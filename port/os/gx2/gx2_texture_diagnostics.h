#pragma once
#include "../../runtime/texture_diagnostics.h"
#ifdef RBREW_TEXTURE_DIAGNOSTICS
#include <cstdint>
namespace gx2vk { struct Image; }
namespace gx2r { struct TexObject; }
namespace txdiag {
void target(uint8_t* mem,const uint8_t* descriptor,gx2vk::Image& image,uint32_t object,const char* kind);
void texture(uint8_t* mem,const uint8_t* descriptor,uint32_t address,uint32_t slot);
void binding(uint32_t program,uint32_t stage,uint32_t unit,uint32_t binding,uint32_t dimension,
             bool complete,gx2r::TexObject* original,gx2r::TexObject* selected,uint64_t view,uint64_t sampler);
void snapshot();
void drain_probes(bool shutdown=false);
void upload(gx2vk::Image& image,uint32_t mip,uint32_t layer,uint32_t layers,const void* data,size_t bytes,uint64_t buffer,uint64_t offset);
struct TextureScope {
    uint8_t* mem;const uint8_t* data;uint32_t address,slot;
    ~TextureScope(){texture(mem,data,address,slot);}
};
}
#endif
