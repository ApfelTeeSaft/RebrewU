#pragma once
// Deliberately separate from ordinary gameplay logging. OFF builds erase the
// entire statement, including argument evaluation. The one-session diagnostic
// build opts in explicitly; merely setting an environment variable cannot
// enable this in the delivered Release binary.
#ifdef RBREW_TEXTURE_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif
namespace txdiag {
template<class T> inline unsigned long long handle(T v) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(v);
    else return static_cast<unsigned long long>(v);
}
inline std::string quote(const char* text) {
    std::string out="\"";
    for (const unsigned char* p=reinterpret_cast<const unsigned char*>(text ? text : ""); *p; ++p) {
        if (*p=='"' || *p=='\\') { out+='\\'; out+=char(*p); }
        else if (*p<32) { char b[7]; std::snprintf(b,sizeof(b),"\\u%04x",*p); out+=b; }
        else out+=char(*p);
    }
    return out+'"';
}
inline uint64_t hash(const void* p,size_t n) {
    uint64_t h=14695981039346656037ull;
    const auto* bytes=static_cast<const uint8_t*>(p);
    for(size_t i=0;i<n;++i)h=(h^bytes[i])*1099511628211ull;
    return h;
}
struct Gate { uint64_t fingerprint=0,frame=0; bool seen=false; };
struct Resource { uint64_t id=0,last_use=0; bool live=false; };
struct Journal {
    std::mutex lock;
    FILE* file=nullptr;
    std::filesystem::path directory;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now(), committed=start;
    uint64_t sequence=0,bytes=0,artifact_bytes=0;
    unsigned part=0;
    bool active=false; // Immutable after construction; file is protected by lock.
    std::atomic<uint64_t> frame{0},serial{0},completed{0},ids{1},epoch{0};
    std::map<std::pair<std::string,uint64_t>,Resource> resources;
    std::unordered_map<std::string,std::unordered_map<uint64_t,Gate>> gates;
    std::array<std::atomic<uint64_t>,16> counts{};
    Journal() {
        const char* path=std::getenv("RBREW_TEXTURE_DIAG_DIR");
        if(path && *path)directory=std::filesystem::u8path(path);
        else {
            std::filesystem::path base=std::filesystem::current_path();
#ifdef _WIN32
            wchar_t executable[32768];
            const DWORD length=GetModuleFileNameW(nullptr,executable,DWORD(std::size(executable)));
            if(length && length<std::size(executable))base=std::filesystem::path(executable).parent_path();
#endif
            const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
            directory=base/"texture-diagnostics"/(std::to_string(stamp)+"-"+
#ifdef _WIN32
                std::to_string(_getpid())
#else
                std::to_string(getpid())
#endif
            );
        }
        std::error_code ec;std::filesystem::create_directories(directory,ec);
        open();
        active=file!=nullptr;
        write("session_start","\"schema\":1,\"sampling_frames\":120,\"cpu_fingerprint\":\"16x64-byte windows per allocation\",\"normal_rendering_behavior\":true");
    }
    void open() {
        const auto path=directory/("textures-"+std::to_string(part++)+".jsonl");
#ifdef _WIN32
        file=_wfopen(path.c_str(),L"wb");
#else
        file=std::fopen(path.c_str(),"wb");
#endif
        bytes=0;
    }
    void flush(bool disk=false) {
        if(!file)return;
        std::fflush(file);
        const auto now=std::chrono::steady_clock::now();
        if(disk || now-committed>=std::chrono::seconds(1)) {
#ifdef _WIN32
            _commit(_fileno(file));
#else
            fsync(fileno(file));
#endif
            committed=now;
        }
    }
    void write(const char* event,const char* fields) {
        if(!file)return;
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        int n=std::fprintf(file,"{\"seq\":%llu,\"ms\":%lld,\"frame\":%llu,\"serial\":%llu,\"completed\":%llu,\"epoch\":%llu,\"event\":\"%s\"%s%s}\n",
            ++sequence,static_cast<long long>(ms),frame.load(),serial.load(),completed.load(),epoch.load(),event,
            fields && *fields ? "," : "",fields ? fields : "");
        if(n>0)bytes+=n;
        flush(); // Every complete event survives process termination/crashes.
        if(bytes>=(128ull<<20)) { flush(true);std::fclose(file);file=nullptr;open(); }
    }
};
// Process exit can terminate render/cache workers without static destruction.
// This diagnostic-only journal intentionally has process lifetime.
inline Journal& journal() { static auto* p=new Journal;return *p; }
inline bool enabled() { return journal().active; }
inline void event(const char* name,const char* format="",...) {
    auto& j=journal();if(!j.active)return;
    char fields[16384];va_list args;va_start(args,format);
    const int n=std::vsnprintf(fields,sizeof(fields),format,args);va_end(args);
    std::lock_guard guard(j.lock);
    if(n<0 || size_t(n)>=sizeof(fields)) { j.write("diagnostic_truncated","\"lost_event\":true");return; }
    j.write(name,fields);
}
inline uint64_t next_id() { return journal().ids.fetch_add(1); }
inline void artifact(const std::string& name,const std::string& contents) {
    auto& j=journal();if(!j.active || contents.empty())return;
    std::lock_guard guard(j.lock);
    if(j.artifact_bytes+contents.size()>(64ull<<20))return;
    const auto path=j.directory/name;
#ifdef _WIN32
    auto* f=_wfopen(path.c_str(),L"wb");
#else
    auto* f=std::fopen(path.c_str(),"wb");
#endif
    if(!f)return;
    const auto written=std::fwrite(contents.data(),1,contents.size(),f);std::fclose(f);
    j.artifact_bytes+=written;
    j.write("shader_artifact",("\"file\":"+quote(name.c_str())+",\"bytes\":"+std::to_string(written)+",\"hash\":"+std::to_string(hash(contents.data(),contents.size()))).c_str());
}
inline uint64_t frame() { return journal().frame.load(); }
inline bool sample(const char* kind,uint64_t key,uint64_t fingerprint=0,uint64_t interval=120) {
    auto& j=journal();if(!j.active)return false;
    std::lock_guard guard(j.lock);auto& g=j.gates[kind][key];const auto f=j.frame.load();
    if(g.seen && g.fingerprint==fingerprint && f-g.frame<interval)return false;
    g={fingerprint,f,true};return true;
}
enum Counter { Lookups,Hits,Misses,Invalidations,Uploads,AliasCopies,DescriptorUpdates,DescriptorHits,Barriers,Submissions,Draws,Skipped,ImageWrites,Count };
inline void count(Counter c) { if(enabled())++journal().counts[c]; }
inline uint64_t resource_id(const char* kind,uint64_t handle) {
    if(!handle)return 0;
    auto& j=journal();std::lock_guard guard(j.lock);
    const auto it=j.resources.find({kind,handle});return it==j.resources.end()?0:it->second.id;
}
inline uint64_t create(const char* kind,uint64_t handle,uint64_t parent=0) {
    if(!enabled() || !handle)return 0;
    auto& j=journal();std::lock_guard guard(j.lock);
    auto& r=j.resources[{kind,handle}];
    if(r.live)j.write("duplicate_live_handle",("\"kind\":"+quote(kind)+",\"id\":"+std::to_string(r.id)).c_str());
    r={next_id(),0,true};
    j.write("resource_create",("\"kind\":"+quote(kind)+",\"handle\":"+std::to_string(handle)+",\"id\":"+std::to_string(r.id)+",\"parent\":"+std::to_string(parent)).c_str());
    return r.id;
}
inline void use(const char* kind,uint64_t handle,uint64_t serial) {
    if(!enabled() || !handle)return;
    auto& j=journal();std::lock_guard guard(j.lock);auto& r=j.resources[{kind,handle}];
    if(!r.live && r.last_use!=serial)j.write("invalid_reference",("\"kind\":"+quote(kind)+",\"handle\":"+std::to_string(handle)+",\"id\":"+std::to_string(r.id)).c_str());
    r.last_use=std::max(r.last_use,serial);
}
inline void retire(const char* kind,uint64_t handle,uint64_t retire_serial) {
    if(!enabled() || !handle)return;
    auto& j=journal();std::lock_guard guard(j.lock);auto& r=j.resources[{kind,handle}];
    j.write("resource_retire",("\"kind\":"+quote(kind)+",\"handle\":"+std::to_string(handle)+",\"id\":"+std::to_string(r.id)+",\"last_use\":"+std::to_string(r.last_use)+",\"retire_serial\":"+std::to_string(retire_serial)+",\"valid\":"+(r.live && retire_serial>=r.last_use?"true":"false")).c_str());
    r.live=false;
}
inline void destroyed(const char* kind,uint64_t handle,uint64_t id,uint64_t completed) {
    event("resource_destroy","\"kind\":%s,\"handle\":%llu,\"id\":%llu,\"gpu_completed\":%llu",quote(kind).c_str(),handle,id,completed);
}
inline void present(uint64_t serial,uint64_t completed) {
    auto& j=journal();if(!j.active)return;
    ++j.frame;j.serial=serial;j.completed=completed;
    if(j.frame%120)return;
    std::string fields="\"counts\":[";
    for(unsigned i=0;i<Count;++i){if(i)fields+=',';fields+=std::to_string(j.counts[i].exchange(0));}
    fields+=']';event("window", "%s",fields.c_str());
}
inline void finish() { event("session_end");auto& j=journal();std::lock_guard guard(j.lock);j.flush(true); }
} // namespace txdiag
#define TXDIAG(...) do { __VA_ARGS__; } while(false)
#else
#define TXDIAG(...) ((void)0)
#endif
