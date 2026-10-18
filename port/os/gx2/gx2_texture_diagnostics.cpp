#include "gx2_texture_diagnostics.h"
#ifdef RBREW_TEXTURE_DIAGNOSTICS
#include "vk/gx2_render_vk_state.h"
#include "gx2_write_watch.h"
#include "../os_common.h"
#include <set>
#include <deque>
namespace txdiag {
namespace {
using namespace gx2r;
struct CpuSignature { uint64_t hash=0; bool valid=true; uint64_t bytes=0; };
// Same four-lane source-byte algorithm as texture_content_hash(), without
// touching renderer counters, write-watch state or upload decisions. Only
// ambiguous aliases use it, with a per-frame byte budget.
struct SourceHash {
    uint64_t lane[4]={0x60EA27EEADC0B5D6ull,0xC2B2AE3D27D4EB4Full,0,0x61C8864E7A143579ull};
    static uint64_t rot(uint64_t v,int n){return v<<n|v>>(64-n);}
    static uint64_t round(uint64_t a,uint64_t v){return rot(a+v*0xC2B2AE3D27D4EB4Full,31)*0x9E3779B185EBCA87ull;}
    void add(const uint8_t* p,size_t n){
        lane[0]=round(lane[0],n);size_t i=0;
        for(;i+32<=n;i+=32){uint64_t w[4];std::memcpy(w,p+i,32);for(unsigned k=0;k<4;++k)lane[k]=round(lane[k],w[k]);}
        unsigned k=1;
        for(;i+8<=n;i+=8,k=(k+1)&3){uint64_t w;std::memcpy(&w,p+i,8);lane[k]=round(lane[k],w);}
        if(i<n){uint64_t w=0;std::memcpy(&w,p+i,n-i);lane[k]=round(lane[k],w);}
    }
    uint64_t finish()const{uint64_t h=rot(lane[0],1)+rot(lane[1],7)+rot(lane[2],12)+rot(lane[3],18);h^=h>>33;h*=0xC2B2AE3D27D4EB4Full;h^=h>>29;h*=0x165667B19E3779F9ull;return h^(h>>32);}
};
void alias_source_hash(uint8_t* mem,const uint8_t* d,const Texture& entry) {
    static uint64_t budget_frame=0,hashed_bytes=0;
    if(budget_frame!=frame()){budget_frame=frame();hashed_bytes=0;}
    const uint64_t bytes=uint64_t(descriptor_word(d,GX2SURF_IMAGE_SIZE))+descriptor_word(d,GX2SURF_MIPMAP_SIZE);
    if(hashed_bytes+bytes>(32u<<20)){event("alias_cpu_hash_deferred","\"cache_id\":%u,\"bytes\":%llu",entry.tex.id,bytes);return;}
    SourceHash hasher;bool valid=true;
    for(auto offset:{GX2SURF_IMAGE_SIZE,GX2SURF_MIPMAP_SIZE}) {
        const auto n=descriptor_word(d,offset),a=descriptor_word(d,offset+4);
        if(!n)continue;
        if(!a || !rbrew_memory_accessible(mem,a,n)){valid=false;break;}
        hasher.add(mem+a,n);hashed_bytes+=n;
    }
    const auto current=hasher.finish();
    event("alias_cpu_hash","\"cache_id\":%u,\"current_full_hash\":%llu,\"last_upload_hash\":%llu,\"valid\":%s,\"matches_last_upload\":%s,\"bytes\":%llu",entry.tex.id,current,entry.content_hash,valid?"true":"false",valid && entry.hash_valid && current==entry.content_hash?"true":"false",bytes);
}
// 16 spaced 64-byte windows per allocation, including interior data. This is
// intentionally independent of GetWriteWatch: observing must not reset dirty
// pages or change the cache's invalidation decisions. Hash is a fingerprint,
// not an allocation-generation oracle; actual mutations get separate ids.
CpuSignature cpu_signature(uint8_t* mem,const uint8_t* d) {
    CpuSignature result;uint64_t h=14695981039346656037ull;
    for(auto offset:{GX2SURF_IMAGE_SIZE,GX2SURF_MIPMAP_SIZE}) {
        const auto n=descriptor_word(d,offset),a=descriptor_word(d,offset+4);
        result.bytes+=n;
        if(!n)continue;
        if(!a || !rbrew_memory_accessible(mem,a,n)){result.valid=false;continue;}
        const uint32_t window=std::min(n,64u),steps=std::min(16u,(n+63u)/64u);
        for(uint32_t i=0;i<steps;++i) {
            const uint64_t at=steps>1?uint64_t(n-window)*i/(steps-1):0;
            const uint64_t part=hash(mem+a+at,window);h=(h^part)*1099511628211ull;
        }
    }
    result.hash=h;return result;
}
std::string hex(const void* p,size_t n) {
    static constexpr char chars[]="0123456789abcdef";std::string out;out.reserve(n*2);
    auto* b=static_cast<const uint8_t*>(p);
    for(size_t i=0;i<n;++i){out+=chars[b[i]>>4];out+=chars[b[i]&15];}return out;
}
struct TargetStamp { uint64_t cpu=0,frame=0,serial=0;uint32_t address=0;bool valid=false; };
std::unordered_map<uint64_t,TargetStamp> targets;
struct TextureAudit { uint64_t cpu=0,generation=0;bool seen=false; };
std::unordered_map<uint64_t,TextureAudit> audits;
struct ProbeRequest { gx2vk::Image* image;uint64_t id; };
struct Probe { gx2vk::Buffer buffer;uint64_t serial,id,frame;gx2vk::ImageDesc desc;VkImageAspectFlags aspect; };
std::deque<ProbeRequest> requested;
std::deque<Probe> pending;
uint64_t raw_bytes=0;
uint64_t image_id(gx2vk::Image* image){return image?resource_id("image",handle(image->image)):0;}
void request_probe(gx2vk::Image* image,uint64_t fingerprint) {
    if(!image || !image->image || requested.size()>=512)return;
    const auto id=image_id(image);
    if(sample("gpu_probe",id,fingerprint,1200))requested.push_back({image,id});
}
}
void target(uint8_t* mem,const uint8_t* d,gx2vk::Image& image,uint32_t object,const char* kind) {
    if(!enabled())return;
    const auto id=image_id(&image);
    if(!sample("target",id,0,120))return;
    const auto cpu=cpu_signature(mem,d);
    targets[id]={cpu.hash,frame(),gx2vk::recording_serial(),descriptor_word(d,GX2SURF_IMAGE),cpu.valid};
    event("target_use","\"kind\":%s,\"object\":%u,\"image_id\":%llu,\"cpu_sample\":%llu,\"cpu_valid\":%s,\"descriptor\":\"%s\"",
          quote(kind).c_str(),object,id,cpu.hash,cpu.valid?"true":"false",hex(d,GX2SURF_SIZEOF).c_str());
}
void texture(uint8_t* mem,const uint8_t* d,uint32_t address,uint32_t slot) {
    if(!enabled())return;
    auto& s=gx2r::state();const auto* entry=s.texture_entries[slot].entry;
    if(!entry)return;
    const uint32_t dim=descriptor_word(d,GX2SURF_DIM),index=dim==3?3:dim==5?1:dim==2?2:0;
    auto* selected=s.units[slot][index];
    const uint64_t selected_id=selected?image_id(selected->image):0;
    // Stable cache entry + slot; return to a previously used route is recorded
    // immediately, while unchanged hit traffic is counted in windows.
    if(!sample("texture",uint64_t(entry->tex.id)*128+slot,selected_id^entry->content_hash^(uint64_t(entry->verify)<<63),120))return;
    const auto cpu=cpu_signature(mem,d);
    auto& audit=audits[entry->tex.id];
    if(!audit.seen || audit.cpu!=cpu.hash) {audit.cpu=cpu.hash;audit.generation=next_id();audit.seen=true;}
    const auto stamp=targets.find(selected_id);
    const bool alias=selected && selected->image!=&entry->image;
    const bool changed=alias && stamp!=targets.end() && stamp->second.valid && cpu.valid && stamp->second.cpu!=cpu.hash;
    event("texture_state","\"address\":%u,\"descriptor_address\":%u,\"slot\":%u,\"cache_id\":%u,\"source_generation\":%llu,\"image_id\":%llu,\"selected_object\":%u,\"alias\":%s,\"target_cpu_changed\":%s,\"target_frame\":%llu,\"target_cpu_sample\":%llu,\"cpu_sample\":%llu,\"cpu_valid\":%s,\"cpu_bytes\":%llu,\"upload_hash\":%llu,\"hash_valid\":%s,\"verify\":%s,\"written\":%llu,\"image_content\":%llu,\"storage\":%s,\"descriptor\":\"%s\"",
        descriptor_word(d,GX2SURF_IMAGE),address,slot,entry->tex.id,audit.generation,selected_id,selected?selected->id:0,
        alias?"true":"false",changed?"true":"false",stamp==targets.end()?0:stamp->second.frame,stamp==targets.end()?0:stamp->second.cpu,
        cpu.hash,cpu.valid?"true":"false",cpu.bytes,entry->content_hash,entry->hash_valid?"true":"false",entry->verify?"true":"false",entry->written,
        selected && selected->image?selected->image->content_generation:0,selected && selected->complete_storage?"true":"false",hex(d,GX2TEX_SIZEOF).c_str());
    if(changed)request_probe(selected->image,cpu.hash);
    if(changed)alias_source_hash(mem,d,*entry);
    // An ambiguous historical match is recorded with ALL candidates, not just
    // the first map-order winner. No change to alias selection is made.
    if(changed || sample("candidates",entry->tex.id,0,600)) {
        const auto a=descriptor_word(d,GX2SURF_IMAGE);
        auto candidates=[&](const auto& map,const char* kind) {
            ColorImageKey low{};low[0]=a;
            for(auto it=map.lower_bound(low);it!=map.end() && it->first[0]==a;++it) {
                const auto& key=it->first;const auto& t=it->second;
                event("alias_candidate","\"cache_id\":%u,\"kind\":%s,\"object\":%u,\"image_id\":%llu,\"content\":%llu,\"key\":[%u,%u,%u,%u,%u,%u,%u,%u,%u]",
                    entry->tex.id,quote(kind).c_str(),t.id,image_id(const_cast<gx2vk::Image*>(&t.image)),t.image.content_generation,
                    key[0],key[1],key[2],key[3],key[4],key[5],key[6],key[7],key[8]);
            }
        };
        candidates(s.cb_map,"color");candidates(s.depth_map,"depth");
    }
}
void binding(uint32_t program,uint32_t stage,uint32_t unit,uint32_t binding,uint32_t dimension,
             bool complete,gx2r::TexObject* original,gx2r::TexObject* selected,uint64_t view,uint64_t sampler) {
    if(!enabled())return;
    if(selected && selected->image)use("image",handle(selected->image->image),gx2vk::recording_serial());
    use("view",view,gx2vk::recording_serial());use("sampler",sampler,gx2vk::recording_serial());
    const uint64_t id=image_id(selected?selected->image:nullptr);
    const uint64_t fingerprint=id^(uint64_t(original?original->id:0)<<32)^view^(sampler*1000003)^complete;
    if(!sample("binding",uint64_t(program)*128+unit,fingerprint,120))return;
    event("binding","\"program\":%u,\"stage\":%u,\"unit\":%u,\"binding\":%u,\"dimension\":%u,\"complete\":%s,\"original_object\":%u,\"selected_object\":%u,\"image_id\":%llu,\"view\":%llu,\"view_id\":%llu,\"sampler\":%llu,\"sampler_id\":%llu,\"content\":%llu",
        program,stage,unit,binding,dimension,complete?"true":"false",original?original->id:0,selected?selected->id:0,id,view,resource_id("view",view),sampler,resource_id("sampler",sampler),selected && selected->image?selected->image->content_generation:0);
}
void snapshot() {
    if(!enabled())return;
    drain_probes();
    auto& s=gx2r::state();
    if(frame()%600==0)request_probe(s.scan_image[0],frame());
    // Queue one transfer at most every six frames, never wait/submit here.
    // Natural presentation/query submissions provide completion. Probe storage
    // is independent of guest images and is freed after its serial completes.
    if(frame()%6==0 && !requested.empty() && pending.size()<4) {
        const auto request=requested.front();requested.pop_front();
        auto& image=*request.image;
        if(image_id(&image)==request.id && image.image && (image.desc.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            const VkImageAspectFlags aspect=(image.aspect&VK_IMAGE_ASPECT_DEPTH_BIT)?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
            const bool bc=gx2vk::format_is_compressed(image.desc.format);
            const auto bytes=uint64_t(bc?(image.desc.width+3)/4:image.desc.width)*(bc?(image.desc.height+3)/4:image.desc.height)*gx2vk::format_block_bytes(image.desc.format,aspect);
            if(bytes && bytes<=(8u<<20)) {
                auto buffer=gx2vk::create_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,gx2vk::MemoryUse::Readback);
                if(buffer.buffer && buffer.memory.mapped) {
                    gx2r::end_rendering();gx2vk::barrier_if_dirty();const auto cb=gx2vk::cmd();
                    VkBufferImageCopy region{};region.imageSubresource={aspect,0,0,1};region.imageExtent={image.desc.width,image.desc.height,1};
                    vkCmdCopyImageToBuffer(cb,image.image,VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&region);
                    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
                    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&host;vkCmdPipelineBarrier2(cb,&dep);
                    // Preserve read-before-later-write execution ordering too.
                    // This is the existing global dependency mechanism; the
                    // image's content generation must not change for a read.
                    gx2vk::note_gpu_write();
                    use("image",handle(image.image),gx2vk::recording_serial());
                    pending.push_back({buffer,gx2vk::recording_serial(),request.id,frame(),image.desc,aspect});
                    event("gpu_probe_queued","\"image_id\":%llu,\"copy_serial\":%llu,\"bytes\":%llu",request.id,gx2vk::recording_serial(),bytes);
                } else {if(buffer.buffer)gx2vk::destroy_buffer(buffer);event("gpu_probe_failed","\"image_id\":%llu",request.id);}
            } else event("gpu_probe_skipped","\"image_id\":%llu,\"bytes\":%llu",request.id,bytes);
        }
    }
    if(frame()%120)return;
    event("renderer_snapshot","\"textures\":%zu,\"colors\":%zu,\"depths\":%zu,\"contexts\":%zu,\"allocations\":%llu,\"images\":%llu,\"validation_errors\":%llu,\"validation_warnings\":%llu,\"scan_object\":%u",
          s.tex_map.size(),s.cb_map.size(),s.depth_map.size(),s.context_snapshots.size(),gx2vk::allocated_bytes(),gx2vk::image_count(),gx2vk::validation_error_count(),gx2vk::validation_warning_count(),s.scan_tv_source_id);
}
void drain_probes(bool shutdown) {
    if(!enabled())return;
    const auto done=gx2vk::known_completed_serial();
    while(!pending.empty() && pending.front().serial<=done) {
        auto p=pending.front();pending.pop_front();
        const std::string name="image-"+std::to_string(p.id)+"-f"+std::to_string(p.frame)+".bin";
        bool saved=false;
        if(raw_bytes+p.buffer.size<=(128ull<<20)) {
            const auto file_path=journal().directory/name;
            if(auto* f=std::fopen(file_path.string().c_str(),"wb")){saved=std::fwrite(p.buffer.memory.mapped,1,size_t(p.buffer.size),f)==p.buffer.size;std::fclose(f);if(saved)raw_bytes+=p.buffer.size;}
        }
        event("gpu_probe","\"image_id\":%llu,\"captured_frame\":%llu,\"copy_serial\":%llu,\"bytes\":%llu,\"hash\":%llu,\"format\":%u,\"width\":%u,\"height\":%u,\"aspect\":%u,\"mip\":0,\"layer\":0,\"raw_file\":%s,\"prefix\":\"%s\"",p.id,p.frame,p.serial,p.buffer.size,hash(p.buffer.memory.mapped,size_t(p.buffer.size)),uint32_t(p.desc.format),p.desc.width,p.desc.height,p.aspect,quote(saved?name.c_str():"").c_str(),hex(p.buffer.memory.mapped,size_t(std::min<uint64_t>(p.buffer.size,256))).c_str());
        vkDestroyBuffer(gx2vk::device(),p.buffer.buffer,nullptr);gx2vk::free_now(p.buffer.memory);
    }
    if(shutdown) {
        for(auto& p:pending)gx2vk::destroy_buffer(p.buffer);
        pending.clear();requested.clear();targets.clear();audits.clear();
    }
}
void upload(gx2vk::Image& image,uint32_t mip,uint32_t layer,uint32_t layers,const void* data,size_t bytes,uint64_t buffer,uint64_t offset) {
    if(!enabled())return;
    const auto id=image_id(&image);use("image",handle(image.image),gx2vk::recording_serial());
    count(Uploads);
    const auto signature=hash(data,bytes);
    if(!sample("upload",id*32+mip,signature,120))return;
    event("upload","\"image_id\":%llu,\"mip\":%u,\"layer\":%u,\"layers\":%u,\"bytes\":%zu,\"hash\":%llu,\"staging_buffer\":%llu,\"staging_offset\":%llu,\"content_before\":%llu",
        id,mip,layer,layers,bytes,signature,buffer,offset,image.content_generation);
}
}
#endif
