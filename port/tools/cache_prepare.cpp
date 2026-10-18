// Offline release preparation. Uses the actual runtime shader compiler/cache
// writer, without a Vulkan device, game entry point or player assets in source.
#include "../os/gx2/vk/gx2_vk_shader.h"
#include <fstream>
#include <iostream>
#include <set>
#include <map>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    gx2vk::shader_set_cache_dir(argv[2]);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    size_t pos = 0;
    auto u32 = [&]() { if (pos + 4 > data.size()) throw std::runtime_error("truncated pack"); uint32_t n; std::memcpy(&n, data.data()+pos,4); pos+=4; return n; };
    try {
        if (u32()!=0x50544252 || u32()!=2) return 3;
        const auto keys=u32(), bodies=u32(); pos += size_t(keys)*12;
        std::vector<uint32_t> sizes;
        for (unsigned i=0;i<bodies;++i) sizes.push_back(u32());
        std::map<std::pair<unsigned,std::string>,bool> unique;
        std::ofstream rejected(argv[3]);
        if(!rejected) return 8;
        unsigned compiled=0, failed=0;
        unsigned body_index=0;
        for (auto size:sizes) {
            const size_t end=pos+size;
            if(end>data.size() || size<28 || u32()!=0x4c544252 || u32()!=2) return 4;
            pos+=8;
            bool body_valid=true;
            for (auto stage:{VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT}) {
                const auto n=u32(); if(pos+n>end) return 5;
                std::string source(reinterpret_cast<const char*>(data.data()+pos),n); pos+=n;
                auto [it,inserted]=unique.emplace(std::make_pair(unsigned(stage),source),false);
                if(inserted) {
                    std::vector<uint32_t> spirv; std::string log;
                    it->second=gx2vk::compile_glsl(stage,source,spirv,&log);
                    if(!it->second) { ++failed; std::cerr<<"Shader failure: "<<log<<'\n'; }
                    else ++compiled;
                }
                body_valid &= it->second;
            }
            if(!body_valid) rejected<<body_index<<'\n';
            ++body_index;
            pos=end;
        }
        std::cout<<"Unique shaders verified: "<<compiled<<", failed: "<<failed<<'\n';
        return failed ? 6 : 0;
    } catch(const std::exception& e) { std::cerr<<e.what(); return 7; }
}
