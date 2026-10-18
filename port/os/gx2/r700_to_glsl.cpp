#include "../../runtime/rbrew_diagnostics.h"
#include "r700_to_glsl.h"
#include "r700_translation_reads.h"
#include "Standalone.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "util/helpers/StringBuf.h"
#include <regex>

thread_local ShaderHostState LatteGPUState;
std::string r700_rect_geometry(const char* vertex_source,bool quad) {
    const std::string vs=vertex_source?vertex_source:"";
    const bool point=vs.find("gl_PointSize;")!=std::string::npos;
    std::string gs=std::string("#version 430\nlayout(")+(quad?"lines_adjacency":"triangles")+") in;layout(triangle_strip,max_vertices=4) out;\nin gl_PerVertex{vec4 gl_Position;";
    if(point)gs+="float gl_PointSize;";
    gs+="}gl_in[];out gl_PerVertex{vec4 gl_Position;};\n";
    struct Varying {std::string name,input;bool flat;};std::vector<Varying> varyings;
    const std::regex output(R"(layout\s*\(location\s*=\s*(\d+)\)\s*((?:flat\s*|noperspective\s*)*)out\s+vec4\s+(\w+)\s*;)");
    for(std::sregex_iterator it(vs.begin(),vs.end(),output),end;it!=end;++it) {
        const auto& m=*it;const std::string input="rbrew_rect_"+m[3].str();
        gs+="layout(location="+m[1].str()+") "+m[2].str()+"in vec4 "+input+"[];\n";
        gs+="layout(location="+m[1].str()+") "+m[2].str()+"out vec4 "+m[3].str()+";\n";
        varyings.push_back({m[3].str(),input,m[2].str().find("flat")!=std::string::npos});
    }
    gs+="void emitCorner(int a,int b,int c,bool generated){\n";
    for(const auto& v:varyings)gs+=v.name+"="+(v.flat?v.input+(quad?"[3]":"[2]"):("generated?"+v.input+"[b]+"+v.input+"[c]-"+v.input+"[a]:"+v.input+"[a]"))+";\n";
    gs+="gl_Position=generated?gl_in[b].gl_Position+gl_in[c].gl_Position-gl_in[a].gl_Position:gl_in[a].gl_Position;EmitVertex();}\n";
    if(quad)gs+="void main(){emitCorner(1,0,0,false);emitCorner(2,0,0,false);emitCorner(0,0,0,false);emitCorner(3,0,0,false);EndPrimitive();}\n";
    else gs+="void main(){float d01=length(gl_in[0].gl_Position.xy-gl_in[1].gl_Position.xy);float d02=length(gl_in[0].gl_Position.xy-gl_in[2].gl_Position.xy);float d12=length(gl_in[1].gl_Position.xy-gl_in[2].gl_Position.xy);int a=0,b=1,c=2;if(d01>d02&&d01>d12){a=2;b=1;c=0;}else if(d02>d01&&d02>d12){a=1;b=2;c=0;}emitCorner(a,b,c,false);emitCorner(b,a,c,false);emitCorner(c,a,b,false);emitCorner(a,b,c,true);EndPrimitive();}\n";
    return gs;
}
static thread_local LatteShaderPSInputTable s_ps_inputs;
LatteShaderPSInputTable* LatteSHRC_GetPSInputTable() { return &s_ps_inputs; }
LatteFetchShader::~LatteFetchShader() = default;

// GX2InitFetchShaderEx converts GX2 format IDs to Latte fetch formats;
// these IDs are distinct from GX2 texture/surface formats.
static constexpr uint32_t fetch_formats[20]={1,2,5,6,7,0xD,0xE,0xF,0x10,0x16,0x1A,0x19,0x1D,0x1E,0x1F,0x20,0x2F,0x30,0x22,0x23};
R700NativeAttribute r700_native_attribute(uint32_t fmt) {
    unsigned raw=fmt&0x3F;
    if (raw>=20) return {};
    // Raw integer inputs preserve bits for Cemu's GLSL attribute decoder.
    const uint32_t types[20]={0x1401,0x1401,0x1403,0x1403,0x1401,0x1405,0x1405,0x1403,0x1403,0x1405,0x1401,0x1405,0x1405,0x1405,0x1403,0x1403,0x1405,0x1405,0x1405,0x1405};
    // Exactly the attribute's bytes (4_4: 1, 8_8: 2, 2_10_10_10: 4): a wider
    // native fetch of a mesh's last vertex reads past its exactly sized
    // vertex buffer, and robust access then zeroes the whole attribute.
    const uint32_t counts[20]={1,1,1,1,2,1,1,2,2,1,4,1,2,2,4,4,3,3,4,4};
    const uint32_t endian[20]={0,0,1,1,0,2,2,1,1,2,0,2,2,2,1,1,2,2,2,2};
    return {types[raw],counts[raw],endian[raw]};
}

void r700_shader_registers(uint32_t* r, const uint8_t* p, bool pixel) {
    auto be = [&](unsigned offset) {
        return uint32_t(p[offset])<<24 | uint32_t(p[offset+1])<<16 |
               uint32_t(p[offset+2])<<8 | p[offset+3];
    };
    // gx2.rpl GX2SetVertexShader (0202fef8), GX2SetPixelShader (02030628).
    if (pixel) {
        r[mmSQ_PGM_RESOURCES_PS] = be(0); r[mmSQ_PGM_EXPORTS_PS] = be(4);
        r[mmSPI_PS_IN_CONTROL_0] = be(8); r[mmSPI_PS_IN_CONTROL_1] = be(12);
        for (unsigned i=0; i<std::min(be(16),32u); ++i) r[mmSPI_PS_INPUT_CNTL_0+i] = be(20+i*4);
        r[mmCB_SHADER_MASK] = be(0x94); r[mmCB_SHADER_CONTROL] = be(0x98);
        r[mmDB_SHADER_CONTROL] = be(0x9C); r[mmSPI_INPUT_Z] = be(0xA0);
    } else {
        r[mmSQ_PGM_RESOURCES_VS] = be(0); r[mmVGT_PRIMITIVEID_EN] = be(4);
        r[mmSPI_VS_OUT_CONFIG] = be(8);
        for (unsigned i=0; i<std::min(be(12),10u); ++i) r[mmSPI_VS_OUT_ID_0+i] = be(16+i*4);
        r[mmPA_CL_VS_OUT_CNTL] = be(0x38); r[mmSQ_VTX_SEMANTIC_CLEAR] = be(0x3C);
        for (unsigned i=0; i<32; ++i) r[mmSQ_VTX_SEMANTIC_0+i] = i<std::min(be(0x40),32u) ? be(0x44+i*4) : 0xFF;
        r[mmVGT_STRMOUT_BUFFER_EN] = be(0xC4);
        // GX2SetVertexShader calls 0202F114 for a stream-out VS, publishing
        // the descriptor's four byte strides as hardware dword strides.
        // Geometry-mode ES binding and non-stream-out VS leave them intact.
        if (be(0xD8)!=2 && be(0x110))
            for (unsigned i=0; i<4; ++i) r[mmVGT_STRMOUT_VTX_STRIDE_0+i*4] = be(0x114+i*4)>>2;
    }
}

void r700_texture_registers(uint32_t* r, const uint8_t* p, uint32_t native_slot) {
    unsigned stage=native_slot/32, slot=native_slot%32;
    if (stage>2 || slot>=18) return;
    unsigned base=(stage==0 ? 0xE000 : stage==1 ? 0xE460 : 0xE930)+slot*7;
    auto be=[&](unsigned a) { return uint32_t(p[a])<<24|uint32_t(p[a+1])<<16|uint32_t(p[a+2])<<8|p[a+3]; };
    // gx2.rpl GX2SetPixelTexture 020362d8 / FUN_020360e0.
    r[base]=be(0x88); r[base+1]=be(0x8C);
    r[base+2]=be(0x24)>>8; r[base+3]=(be(0x2C)?be(0x2C):be(0x24))>>8;
    r[base+4]=be(0x90); r[base+5]=be(0x94); r[base+6]=be(0x98);
}

static thread_local R700TranslationReads s_last_reads;
R700TranslationReads r700_last_translation_reads() { return s_last_reads; }

char* r700_to_glsl(const R700TranslateInput& in) {
    s_last_reads = {};
    if (!in.data || !in.size || (in.size&7)) return nullptr;
    LatteDecompilerOutput_t out{};
    try {
        // An aligned private copy avoids aliasing and unaligned host reads.
        std::vector<uint32_t> words(in.size/4);
        std::memcpy(words.data(),in.data,in.size);
        std::array<uint32_t, 0x10000> regs{};
        if (in.context_registers) std::copy_n(in.context_registers,regs.size(),regs.data());
        LatteShader_CreatePSInputTable(&s_ps_inputs,regs.data());
        LatteFetchShader fetch;
        std::vector<LatteParsedFetchShaderAttribute> attrs;
        attrs.reserve(in.attributes.size());
        for (const auto& a : in.attributes) {
            if (a.semantic>=256 || a.buffer>=16) throw std::runtime_error("Invalid GX2 attribute");
            LatteParsedFetchShaderAttribute attr{};
            attr.semanticId=uint8_t(a.semantic); attr.attributeBufferIndex=uint8_t(a.buffer);
            if ((a.format&0x3F)>=20) throw std::runtime_error("Unsupported GX2 attribute format");
            attr.offset=a.offset; attr.format=Latte::E_HWFMT(fetch_formats[a.format&0x3F]);
            attr.nfa=(a.format&0x800) ? 2 : (a.format&0x100) ? 1 : 0;
            attr.isSigned=(a.format&0x200) != 0;
            unsigned endian=a.endian==3 ? r700_native_attribute(a.format).default_endian : a.endian;
            attr.endianSwap=LatteConst::VertexFetchEndianMode(endian);
            attr.fetchType=a.per_instance ? LatteConst::INSTANCE_DATA : LatteConst::VERTEX_DATA;
            attr.aluDivisor=a.divisor;
            for (unsigned i=0;i<4;++i) attr.ds[i]=uint8_t(a.destination_select>>(24-i*8));
            attrs.push_back(attr);
        }
        for (auto& attr : attrs) {
            LatteParsedFetchShaderBufferGroup group{};
            group.attributeBufferIndex=attr.attributeBufferIndex; group.attribCount=1; group.attrib=&attr;
            fetch.bufferGroups.push_back(group);
        }
        LatteDecompilerOptions options{};
        options.strictMul=true;
        options.nativeVulkan=true;
        options.nativeVulkanNegativeOneToOne=in.vulkan_negative_one_to_one;
        if (in.is_pixel) LatteDecompiler_DecompilePixelShader(0,regs.data(),reinterpret_cast<uint8_t*>(words.data()),in.size,options,&out);
        else LatteDecompiler_DecompileVertexShader(0,regs.data(),reinterpret_cast<uint8_t*>(words.data()),in.size,&fetch,options,&out);
        if (!out.shader || out.shader->hasError || !out.shader->strBuf_shaderSource)
            throw std::runtime_error("Latte shader translation failed");
        if (in.info) {
            auto& info=*in.info;
            info.remapped.clear();
            for (auto& e : out.shader->list_remappedUniformEntries)
                info.remapped.push_back({e.isRegister,e.kcacheBankId,e.index,e.mappedIndex});
            std::copy_n(out.sourceMapping.attributeMapping,256,info.attribute_locations.begin());
            std::copy_n(out.sourceMapping.uniformBuffersBindingPoint,16,info.buffer_bindings.begin());
            info.uniform_mode=out.shader->uniformMode;
            info.register_count=out.uniformOffsetsVK.count_uniformRegister;
            std::copy_n(out.shader->textureUsesDepthCompare,18,info.texture_compare.begin());
            info.streamout_mask=uint32_t(out.streamoutBufferWriteMask.to_ulong());
            std::copy_n(out.streamoutBufferStride,4,info.streamout_strides.begin());
            info.streamout_written_words=out.streamoutWrittenWords;
            auto& vk=info.vk;
            const auto& map=out.resourceMappingVK;
            const auto& offsets=out.uniformOffsetsVK;
            vk=R700VulkanInterface{};
            vk.set=map.setIndex;
            vk.uniform_vars_binding=map.uniformVarsBufferBindingPoint;
            std::copy_n(map.uniformBuffersBindingPoint,16,vk.uniform_buffer_bindings.begin());
            for(const auto& entry:out.shader->list_quickBufferList)
                if(entry.index<16)vk.uniform_buffer_vec4s[entry.index]=entry.size/16;
            for(unsigned t=0;t<18;++t) {
                // -1 whenever the unit does not fetch texels (upstream default),
                // so a zero offset can never be mistaken for the first uniform.
                vk.offset_tex_scale[t]=offsets.offset_texScale[t];
                vk.texture_bindings[t]=out.textureUnitMask[t]?map.textureUnitToBindingPoint[t]:-1;
                if(!out.textureUnitMask[t])continue;
                const auto dim=out.shader->textureUnitDim[t];
                vk.texture_dim[t]=dim==Latte::E_DIM::DIM_1D?1:dim==Latte::E_DIM::DIM_2D || dim==Latte::E_DIM::DIM_2D_MSAA?2:
                    dim==Latte::E_DIM::DIM_2D_ARRAY?3:dim==Latte::E_DIM::DIM_CUBEMAP?4:dim==Latte::E_DIM::DIM_3D?5:0;
                vk.texture_integer[t]=out.shader->textureIsIntegerFormat[t];
                vk.texture_shadow[t]=out.shader->textureUsesDepthCompare[t];
            }
            vk.offset_remapped=offsets.offset_remapped;
            vk.offset_registers=offsets.offset_uniformRegister;
            vk.register_vec4s=offsets.count_uniformRegister;
            vk.offset_alpha_ref=offsets.offset_alphaTestRef;
            vk.offset_point_size=offsets.offset_pointSize;
            vk.offset_frag_coord_scale=offsets.offset_fragCoordScale;
            vk.offset_window_to_clip=offsets.offset_windowSpaceToClipSpaceTransform;
            vk.uniform_block_bytes=uint32_t(std::max(0,offsets.offset_endOfBlock));
            vk.pixel_outputs=out.shader->pixelColorOutputMask;
        }
        s_last_reads.valid=true;
        s_last_reads.vs_parameter_mask=in.is_pixel?0:out.shader->outputParameterMask;
        for(unsigned t=0;t<LATTE_NUM_MAX_TEX_UNITS && t<32;++t)
            if(out.textureUnitMask[t])s_last_reads.texture_unit_mask|=1u<<t;
        std::string translated=out.shader->strBuf_shaderSource->c_str();
        if(!in.is_pixel) {
            const auto line=translated.find('\n');
            translated.insert(line+1,"layout(push_constant) uniform RbrewPushConstants { int rbrew_base_instance; };\n");
        }
        const auto* src=translated.c_str();
        char* result=static_cast<char*>(std::malloc(std::strlen(src)+1));
        if (result) std::strcpy(result,src);
        delete out.shader->strBuf_shaderSource; delete out.shader;
        return result;
    } catch (const std::exception& error) {
        RBREW_LOGF(stderr,"[shader] %s\n",error.what());
        if (out.shader) { delete out.shader->strBuf_shaderSource; delete out.shader; }
        return nullptr;
    }
}

std::string r700_vulkan_integer_outputs(const std::string& pixel_source,uint32_t uint_mask,uint32_t sint_mask) {
    std::string source=pixel_source;
    std::string epilogue;
    for(unsigned i=0;i<8;++i) {
        const bool is_uint=(uint_mask>>i)&1,is_sint=(sint_mask>>i)&1;
        if(!is_uint && !is_sint)continue;
        const std::string declaration="layout(location = "+std::to_string(i)+") out vec4 passPixelColor"+std::to_string(i)+";";
        const auto at=source.find(declaration);
        if(at==std::string::npos)continue;   // output not written: the attachment write mask is zero
        const std::string out="rbrew_pixel_color"+std::to_string(i);
        source.replace(at,declaration.size(),"vec4 passPixelColor"+std::to_string(i)+" = vec4(0.0);\nlayout(location = "+
            std::to_string(i)+") out "+(is_uint?"uvec4 ":"ivec4 ")+out+";");
        epilogue+=out+" = "+(is_uint?"floatBitsToUint(":"floatBitsToInt(")+"passPixelColor"+std::to_string(i)+");\n";
    }
    if(epilogue.empty())return source;
    const auto main=source.find("void main()");
    if(main==std::string::npos)return source;
    source.replace(main,11,"void rbrew_main()");
    source+="\nvoid main()\n{\nrbrew_main();\n"+epilogue+"}\n";
    return source;
}

std::string r700_vulkan_capture_position(const std::string& input) {
    std::string source=input;
    const auto block=source.find("out gl_PerVertex");
    if(block==std::string::npos)return source;
    const auto position=source.find("vec4 gl_Position;",block);
    if(position==std::string::npos)return source;
    source.insert(position,"layout(xfb_offset = 0) ");
    source.insert(block,"layout(xfb_buffer = 0, xfb_stride = 16) ");
    // xfb qualifiers are core in GLSL 4.40; the translator emits 4.30.
    const auto version=source.find("#version");
    if(version!=std::string::npos) {
        const auto eol=source.find('\n',version);
        if(eol!=std::string::npos)source.insert(eol+1,"#extension GL_ARB_enhanced_layouts : require\n");
    }
    return source;
}
