#include "../../runtime/rbrew_diagnostics.h"
#include "erreula.h"
#include "../coreinit/coreinit_dynload.h"
#include <array>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {
struct StateOwner {
    std::mutex mutex;
    std::unordered_map<uint8_t*,RbrewErrEulaSnapshot> states;
};
StateOwner& state_owner() {
    // Active native guest workers can query the provider during process exit.
    // Individual arena lifetimes end explicitly after worker quiescence.
    static auto* owner = new StateOwner;
    return *owner;
}
[[noreturn]] void unsupported(const char* api) {
    RBREW_LOGF(stderr,"[erreula] unsupported active capability: %s\n",api);
    throw std::runtime_error(std::string("ErrEula active UI capability unavailable: ")+api);
}
bool guest_pointer(CPUState* cpu,uint32_t value) {
    return cpu->mem && value && value<cpu->guest_memory_size &&
        cpu->guest_memory_size<=WIIU_MEM_SIZE && rbrew_memory_accessible(cpu->mem,value,1);
}
void set_version(CPUState* cpu) {
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    owner.states[cpu->mem].version=cpu->r[3]; // void: preserve the guest registers
}
void create(CPUState* cpu) {
    if (!guest_pointer(cpu,cpu->r[3]) || !guest_pointer(cpu,cpu->r[6]))
        throw std::runtime_error("ErrEulaCreate requires valid work memory and FSClient");
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    auto& state=owner.states[cpu->mem];
    if (state.initialized) throw std::runtime_error("ErrEulaCreate called with a live instance");
    const uint32_t version=state.version;
    state={}; state.version=version;
    state.work_memory=cpu->r[3]; state.region=cpu->r[4];
    state.language=cpu->r[5]; state.fs_client=cpu->r[6];
    state.initialized=true;
    // WUT State::Hidden/ResultType::None and original Calc/Draw guards; no
    // fabricated success return or guest object-field initialization.
    RBREW_LOGF(stderr,"[erreula] initialized version=%u work=%08X region=%u language=%u FSClient=%08X state=Hidden\n",
        state.version,state.work_memory,state.region,state.language,state.fs_client);
}
void destroy(CPUState* cpu) {
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    auto& state=owner.states[cpu->mem];
    const uint32_t version=state.version; state={}; state.version=version;
}
void get_state(CPUState* cpu) { cpu->r[3]=erreula_snapshot(cpu).viewer_state; }
void get_home(CPUState* cpu) { cpu->r[3]=erreula_snapshot(cpu).home_nix; }
void get_result(CPUState* cpu) { cpu->r[3]=uint32_t(erreula_snapshot(cpu).result_code); }
void get_result_type(CPUState* cpu) {
    const auto state=erreula_snapshot(cpu);
    if (state.result_code!=-1) unsupported("GetResultType mapping");
    cpu->r[3]=0; // ResultType::None for no decision; active mappings not guessed
}
void get_button(CPUState* cpu) { cpu->r[3]=uint32_t(erreula_snapshot(cpu).selected_button); }
void get_decision(CPUState* cpu) { cpu->r[3]=erreula_snapshot(cpu).selected_button!=-1; }
void get_left(CPUState* cpu) { cpu->r[3]=erreula_snapshot(cpu).selected_button==0; }
void get_right(CPUState* cpu) { cpu->r[3]=erreula_snapshot(cpu).selected_button==1; }
void get_cursor(CPUState* cpu) {
    if (erreula_snapshot(cpu).viewer_state!=0) unsupported("IsSelectCursorActive input");
    cpu->r[3]=0;
}
void change_language(CPUState* cpu) { auto& owner=state_owner(); std::lock_guard lock(owner.mutex); owner.states[cpu->mem].language=cpu->r[3]; }
void set_controller(CPUState* cpu) { auto& owner=state_owner(); std::lock_guard lock(owner.mutex); owner.states[cpu->mem].controller=cpu->r[3]; }
void set_sound(CPUState* cpu) { auto& owner=state_owner(); std::lock_guard lock(owner.mutex); owner.states[cpu->mem].play_appear_sound=cpu->r[3]!=0; }
void disappear_home(CPUState* cpu) { auto& owner=state_owner(); std::lock_guard lock(owner.mutex); owner.states[cpu->mem].home_nix=false; }
void disappear_error(CPUState* cpu) {
    if (erreula_snapshot(cpu).viewer_state!=0) unsupported("DisappearError animation");
}
void inactive_calc(CPUState* cpu) {
    const auto state=erreula_snapshot(cpu);
    if (state.viewer_state!=0 || state.home_nix) unsupported("Calc/input/fade");
}
void inactive_draw(CPUState* cpu) {
    const auto state=erreula_snapshot(cpu);
    if (state.viewer_state!=0 || state.home_nix) unsupported("Draw/render");
}
void appear_error(CPUState* cpu) {
    // Diagnostic: nn::erreula::ErrorArg errorType @+0x00, errorCode @+0x10.
    if (guest_pointer(cpu,cpu->r[3]) && rbrew_memory_accessible(cpu->mem,cpu->r[3],0x14))
        RBREW_LOGF(stderr,"[erreula] AppearError type=%u code=%d lr=%08X caller_lr=%08X\n",
            rbrew_read32(cpu->mem,cpu->r[3]),int32_t(rbrew_read32(cpu->mem,cpu->r[3]+0x10)),cpu->lr,
            guest_pointer(cpu,cpu->r[1]+0x14) ? rbrew_read32(cpu->mem,cpu->r[1]+0x14) : 0u);
    unsupported("AppearError");
}
void appear_home(CPUState*) { unsupported("AppearHomeNixSign"); }
void jump(CPUState*) { unsupported("Jump"); }
struct Export { const char* name; HostFunc handler; };
constexpr Export exports[]{
    {"ErrEulaSetVersion__3RplFi",set_version},
    {"ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient",create},
    {"ErrEulaDestroy__3RplFv",destroy},
    {"ErrEulaGetStateErrorViewer__3RplFv",get_state},
    {"ErrEulaIsAppearHomeNixSign__3RplFv",get_home},
    {"ErrEulaGetResultCode__3RplFv",get_result},
    {"ErrEulaGetResultType__3RplFv",get_result_type},
    {"ErrEulaGetSelectButtonNumError__3RplFv",get_button},
    {"ErrEulaIsDecideSelectButtonError__3RplFv",get_decision},
    {"ErrEulaIsDecideSelectLeftButtonError__3RplFv",get_left},
    {"ErrEulaIsDecideSelectRightButtonError__3RplFv",get_right},
    {"ErrEulaIsSelectCursorActive__3RplFv",get_cursor},
    {"ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType",change_language},
    {"ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType",set_controller},
    {"ErrEulaPlayAppearSE__3RplFb",set_sound},
    {"ErrEulaDisappearHomeNixSign__3RplFv",disappear_home},
    {"ErrEulaDisappearError__3RplFv",disappear_error},
    {"ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo",inactive_calc},
    {"ErrEulaDrawTV__3RplFv",inactive_draw},
    {"ErrEulaDrawDRC__3RplFv",inactive_draw},
    {"ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg",appear_error},
    {"ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg",appear_home},
    {"ErrEulaJump__3RplFPCcUi",jump},
};
}

void erreula_register(CPUState* cpu,uint32_t thunk_begin) {
    if (!cpu || !cpu->dispatch_table || (thunk_begin&3u) || !thunk_begin ||
        uint64_t(thunk_begin)/4+std::size(exports)>cpu->dispatch_size)
        throw std::runtime_error("ErrEula native export namespace does not fit dispatch table");
    std::array<RbrewDynLoadExport,std::size(exports)> declarations;
    // Validate all namespace collisions before publishing any function.
    for (size_t i=0;i<std::size(exports);++i) {
        auto previous=cpu->dispatch_table[(thunk_begin>>2)+i];
        if (previous && previous!=exports[i].handler)
            throw std::runtime_error("ErrEula native export namespace collision");
    }
    for (size_t i=0;i<std::size(exports);++i) {
        const uint32_t address=thunk_begin+uint32_t(i)*4;
        rbrew_register_func(cpu,address,exports[i].handler);
        declarations[i]={exports[i].name,RbrewDynLoadExportKind::Function,address};
    }
    rbrew_dynload_register_module(cpu,"erreula.rpl",declarations);
}
RbrewErrEulaSnapshot erreula_snapshot(const CPUState* cpu) {
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex);
    auto found=owner.states.find(cpu->mem);
    return found==owner.states.end() ? RbrewErrEulaSnapshot{} : found->second;
}
void erreula_forget_arena(const CPUState* cpu) {
    auto& owner=state_owner();
    std::lock_guard lock(owner.mutex); owner.states.erase(cpu->mem);
}
