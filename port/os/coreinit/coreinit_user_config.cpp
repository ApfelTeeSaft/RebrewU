#include "coreinit_user_config.h"
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace {
struct ArenaConfig {
    RbrewUcSystemProfile profile;
    std::unordered_set<uint32_t> handles;
};
struct ConfigOwner {
    std::mutex mutex;
    std::unordered_map<const uint8_t*,ArenaConfig> arenas;
    uint32_t next_handle{1};
};
ConfigOwner& config_owner() {static auto* owner=new ConfigOwner;return *owner;}
void require_arena(const CPUState* cpu) {
    if(!cpu || !cpu->mem || !cpu->guest_memory_size || cpu->guest_memory_size>WIIU_MEM_SIZE)
        throw std::runtime_error("UC requires a valid guest arena");
}
ArenaConfig& require_config(ConfigOwner& owner,const CPUState* cpu) {
    const auto found=owner.arenas.find(cpu->mem);
    if(found==owner.arenas.end())throw std::runtime_error("Native UC profile is not configured");
    return found->second;
}
void require_handle(const ArenaConfig& config,uint32_t handle) {
    if(!config.handles.contains(handle))
        throw std::runtime_error("UC session is invalid, closed, or belongs to another guest arena");
}
}
void rbrew_uc_configure_profile(CPUState* cpu,const RbrewUcSystemProfile& profile) {
    require_arena(cpu);
    if(profile.language>11 || profile.country_registration>255)
        throw std::invalid_argument("Unsupported native UC language or country-registration value");
    auto& owner=config_owner();std::lock_guard lock(owner.mutex);
    auto found=owner.arenas.find(cpu->mem);
    if(found!=owner.arenas.end() && !found->second.handles.empty() && found->second.profile!=profile)
        throw std::runtime_error("Cannot replace a native UC profile while sessions are open");
    owner.arenas[cpu->mem].profile=profile;
}
void rbrew_uc_forget_arena(const CPUState* cpu) {
    if(!cpu || !cpu->mem)return;
    auto& owner=config_owner();std::lock_guard lock(owner.mutex);owner.arenas.erase(cpu->mem);
}

void rbrew_uc_open(CPUState* cpu) {
    require_arena(cpu);auto& owner=config_owner();std::lock_guard lock(owner.mutex);
    auto& config=require_config(owner,cpu);
    if(owner.next_handle>uint32_t(std::numeric_limits<int32_t>::max()))
        throw std::runtime_error("Native UC session namespace exhausted");
    const uint32_t handle=owner.next_handle;
    config.handles.insert(handle);++owner.next_handle;cpu->r[3]=handle;
}
void rbrew_uc_close(CPUState* cpu) {
    require_arena(cpu);auto& owner=config_owner();std::lock_guard lock(owner.mutex);
    auto& config=require_config(owner,cpu);require_handle(config,cpu->r[3]);
    config.handles.erase(cpu->r[3]);cpu->r[3]=0;
}
void rbrew_uc_read_sys_config(CPUState* cpu) {
    require_arena(cpu);
    RbrewUcSystemProfile profile;
    {
        auto& owner=config_owner();std::lock_guard lock(owner.mutex);
        const auto& config=require_config(owner,cpu);require_handle(config,cpu->r[3]);
        profile=config.profile;
    }
    constexpr uint32_t item_size=0x54;
    constexpr uint32_t key_not_found=uint32_t(-0x200009),invalid_type=uint32_t(-0x200006),
                       invalid_param=uint32_t(-0x200005);
    const uint32_t count=cpu->r[4],base=cpu->r[5];
    const uint64_t bytes=uint64_t(count)*item_size;
    auto accessible=[&](uint32_t address,uint64_t length) {
        return address && uint64_t(address)+length<=cpu->guest_memory_size &&
               rbrew_memory_accessible(cpu->mem,address,length);
    };
    if(!accessible(base,bytes))throw std::runtime_error("UC descriptor array is outside accessible guest memory");
    // UCDataType: 1 = unsigned byte, 3 = unsigned int (wut coreinit/userconfig.h).
    constexpr uint32_t type_u8=1,type_u32=3;
    struct Item {uint32_t descriptor,output,value,error,width;};
    std::vector<Item> items;items.reserve(count);
    // Parse and validate the entire batch before guest publication. Malformed
    // storage and unproved alias/permission policies fail explicitly; these
    // host exceptions are not presented as invented Cafe status codes.
    for(uint32_t i=0;i<count;++i) {
        const uint32_t at=base+i*item_size;
        std::string name;
        for(uint32_t j=0;j<64;++j) {
            const auto byte=rbrew_read8(cpu->mem,at+j);
            if(!byte)break;
            name.push_back(char(byte));
        }
        if(name.size()==64)throw std::runtime_error("UC key is not terminated within its packed name field");
        if(rbrew_read32(cpu->mem,at+0x40)!=0x777)
            throw std::runtime_error("Native UC read access policy is unsupported");
        Item item{at,rbrew_read32(cpu->mem,at+0x50),0,0,4};
        uint32_t type=type_u32;
        if(name=="cafe.language")item.value=profile.language;
        else if(name=="cafe.cntry_reg")item.value=profile.country_registration;
        // Parental settings of the slot-1 parental account (the slot nn::act
        // GetParentalControlSlotNoEx reports). decaf-emu's default parental.xml
        // and Cemu both publish unsigned-byte 0 = unrestricted.
        else if(name=="p_acct1.net_communication_on_game" || name=="p_acct1.network_launcher" ||
                name=="p_acct1.int_movie" || name=="p_acct1.int_browser") {
            item.width=1;type=type_u8;
        }
        else item.error=key_not_found;
        if(!item.error && rbrew_read32(cpu->mem,at+0x44)!=type)item.error=invalid_type;
        if(!item.error && (!item.output || rbrew_read32(cpu->mem,at+0x4C)<item.width))item.error=invalid_param;
        if(!item.error) {
            if(!accessible(item.output,item.width))throw std::runtime_error("UC scalar output is outside accessible guest memory");
            if(uint64_t(item.output)<uint64_t(base)+bytes && uint64_t(item.output)+item.width>base)
                throw std::runtime_error("UC scalar output overlaps descriptor storage");
            for(const auto& earlier:items)
                if(!earlier.error && uint64_t(item.output)<uint64_t(earlier.output)+earlier.width &&
                   uint64_t(item.output)+item.width>earlier.output)
                    throw std::runtime_error("UC scalar output destinations overlap");
        }
        items.push_back(item);
    }
    uint32_t result=0;
    for(const auto& item:items) {
        rbrew_write32(cpu->mem,item.descriptor+0x48,item.error);
        if(item.error)result=item.error;
        else if(item.width==1)rbrew_write8(cpu->mem,item.output,uint8_t(item.value));
        else rbrew_write32(cpu->mem,item.output,item.value);
    }
    cpu->r[3]=result;
}
