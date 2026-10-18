#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/texture_diagnostics.h"
// coreinit filesystem: FSInit, FSOpenFile, FSReadFile, etc. — host-backed I/O
#include "../os_common.h"
#include "coreinit_fs.h"
#include "../../runtime/native_control_defaults.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#ifdef _WIN32
#  include "../../dirent_win.h"
#  include <direct.h>
#  include <io.h>
#  define mkdir(p,m) _mkdir(p)
#else
#  include <dirent.h>
#  include <unistd.h>
#endif
#include <unordered_map>
#include <mutex>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <algorithm>
#include <stdexcept>

// FS error codes (subset)
#define FS_STATUS_OK              0
#define FS_STATUS_END             -4
#define FS_STATUS_NOT_FOUND       -6
#define FS_STATUS_ACCESS_ERROR    -16
#define FS_STATUS_PERMISSION_ERROR -14
#define FS_STATUS_FATAL_ERROR     -128

// FSAsyncData guest layout (for async callbacks — we run sync and ignore them)
// r3=client r4=block r5=path r6=mode r7=flags r8=handle_out r9=async

static std::mutex       s_fs_mutex;
static FILE*            s_file_handles[256] = {};
static bool             s_file_writable[256] = {};
static uint8_t          s_file_save_slot[256] = {};
static std::filesystem::path s_file_save_path[256];
static DIR*             s_dir_handles[256]  = {};
static int32_t          s_last_error        = FS_STATUS_OK;

static bool guest_range(CPUState* cpu,uint32_t address,uint64_t size) {
    return uint64_t(address)+size<=cpu->guest_memory_size &&
        rbrew_memory_accessible(MEM,address,size);
}

static void require_guest_range(CPUState* cpu,uint32_t address,uint64_t size) {
    if (!guest_range(cpu,address,size))
        throw std::runtime_error("FS guest buffer is outside accessible memory");
}

static const char* guest_string(CPUState* cpu,uint32_t address) {
    if (!address) throw std::runtime_error("FS guest string is null");
    for(uint64_t cursor=address;cursor<cpu->guest_memory_size;++cursor) {
        if (!rbrew_memory_accessible(MEM,uint32_t(cursor),1)) break;
        if (!MEM[cursor]) return reinterpret_cast<const char*>(MEM+address);
    }
    throw std::runtime_error("FS guest string is unterminated or outside accessible memory");
}

// Map guest content path prefix to host path
// /vol/storage_mlc01 is the shared system volume (Mii resources and shared
// (sys/ paths use RBREW_SYSTEM_MLC_DIR when set, e.g. wiiu-system/mlc)
// save data such as FFL_ODB.dat); it maps to RBREW_MLC_DIR, default ./mlc01.
// All FS opens are read-only, so a configured host mlc is never modified.
static const char* remap_path(const char* guest, char* out, size_t outsz) {
    const char* root_env = "RBREW_CONTENT_DIR";
    const char* root_default = "./content";
    if (strncmp(guest, "/vol/content", 12) == 0 &&
        (guest[12] == '/' || guest[12] == '\0')) guest += 12;
    else if (strncmp(guest, "content:", 8) == 0)  guest += 8;
    else if (strncmp(guest, "/vol/storage_mlc01", 18) == 0 &&
             (guest[18] == '/' || guest[18] == '\0')) {
        guest += 18; root_env = "RBREW_MLC_DIR"; root_default = "./mlc01";
    }
    else if (guest[0] == '/') return nullptr; // other volumes are not implemented
    while (*guest == '/' || *guest == '\\') ++guest;
    std::filesystem::path relative(guest);
    if (relative.is_absolute() || relative.has_root_name()) return nullptr;
    for (const auto& part : relative) if (part == "..") return nullptr;
    // System titles (sys/..., e.g. the Mii resources 0005001b-10056000) may
    // come from a dumped console MLC while saves stay in RBREW_MLC_DIR.
    if (std::strcmp(root_env, "RBREW_MLC_DIR") == 0 && !relative.empty() &&
        *relative.begin() == "sys") {
        const char* system_root = std::getenv("RBREW_SYSTEM_MLC_DIR");
        if (system_root && *system_root) root_env = "RBREW_SYSTEM_MLC_DIR";
    }
    const char* configured = std::getenv(root_env);
    std::string path = (std::filesystem::path(configured && *configured
        ? configured : root_default) / relative).string();
    if (path.size() >= outsz) return nullptr;
    std::memcpy(out, path.c_str(), path.size() + 1);
    return out;
}

static uint32_t alloc_file_handle(FILE* f, bool writable = false, uint8_t save_slot = 0,
                                  const std::filesystem::path& save_path = {}) {
    for (int i = 1; i < 256; i++) if (!s_file_handles[i]) {
        s_file_handles[i] = f; s_file_writable[i] = writable; s_file_save_slot[i] = save_slot;
        s_file_save_path[i] = save_path;
        return (uint32_t)i;
    }
    return 0;
}
static uint32_t alloc_dir_handle(DIR* d) {
    for (int i = 1; i < 256; i++) if (!s_dir_handles[i]) { s_dir_handles[i] = d; return (uint32_t)i; }
    return 0;
}

static void FSInit(CPUState* cpu) {
    (void)cpu; s_last_error = FS_STATUS_OK;
    const char* root = std::getenv("RBREW_CONTENT_DIR");
    RBREW_LOGF(stderr, "[FS] Content root: %s (read-only)\n", root ? root : "./content");
}
static void FSShutdown(CPUState* cpu) { (void)cpu; }
static void FSAddClient(CPUState* cpu) { RET = (uint32_t)FS_STATUS_OK; }
static void FSDelClient(CPUState* cpu) { RET = (uint32_t)FS_STATUS_OK; }
static void FSInitCmdBlock(CPUState* cpu) {
    // Zero the FSCmdBlock pointed to by r3
    if (ARG0) {
        require_guest_range(cpu,ARG0,0xA80);
        memset(MEM + ARG0, 0, 0xA80);
    }
}
static void FSSetCmdPriority(CPUState* cpu) { (void)cpu; }
static void FSSetStateChangeNotification(CPUState* cpu) { (void)cpu; }
// coreinit.rpl 0204d468: the client's volume state, or 0xB (INVALID) when the
// client is unknown. Clients are not tracked here; any client sees its
// volumes mounted: FS_VOLSTATE_READY = 1 (0 is INITIAL, which the game's
// FS check sequence treats as an error and reports via ErrEula).
static void FSGetVolumeState(CPUState* cpu) { RET = ARG0 ? 1u : 0xBu; }
static void FSFlushQuota(CPUState* cpu) { RET = (uint32_t)FS_STATUS_OK; }

static void FSGetLastError(CPUState* cpu) {
    // r3=client, r4=block -> return last FSStatus in r3
    RET = (uint32_t)s_last_error;
}
static void FSGetLastErrorCodeForViewer(CPUState* cpu) { RET = (uint32_t)s_last_error; }

static void FSGetCwd(CPUState* cpu) {
    // r3=client r4=block r5=buf r6=len r7=async
    uint32_t buf = ARG2, len = ARG3;
    const char* cwd = "/vol/content";
    if (buf && len) {
        require_guest_range(cpu,buf,len);
        strncpy((char*)(MEM + buf), cwd, len);
        MEM[buf + len - 1] = '\0';
    }
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSMakeDir(CPUState* cpu) {
    RET = (uint32_t)FS_STATUS_PERMISSION_ERROR;
}

static void FSOpenFile(CPUState* cpu) {
    // r3=client r4=block r5=path r6=mode r7=handle_out r8=flags r9=async
    char host[512];
    const char* gpath = guest_string(cpu,ARG2);
    const char* mode  = guest_string(cpu,ARG3);
    uint32_t handle_out = ARG4;
    if (handle_out) require_guest_range(cpu,handle_out,4);
    if (!remap_path(gpath, host, sizeof(host)) ||
        mode[0] != 'r' || strchr(mode, '+')) {
        RET = (uint32_t)FS_STATUS_PERMISSION_ERROR;
        return;
    }
    FILE* f = fopen(host, "rb");
    TXDIAG(txdiag::event("asset_open","\"path\":%s,\"ok\":%s,\"lr\":%u",txdiag::quote(gpath).c_str(),f?"true":"false",cpu->lr));
    if (!f) {
        RBREW_LOGF(stderr, "[FS] FSOpenFile failed: %s (errno=%d)\n", host, errno);
        s_last_error = FS_STATUS_NOT_FOUND;
        RET = (uint32_t)FS_STATUS_NOT_FOUND;
        return;
    }
    if (rbrew_diagnostic_env("RBREW_FS_VERBOSE")) RBREW_LOGF(stderr, "[FS] Open: %s\n", host);
    uint32_t h = alloc_file_handle(f);
    TXDIAG(txdiag::create("file",h);txdiag::event("asset_handle","\"path\":%s,\"handle\":%u,\"file_id\":%llu",txdiag::quote(gpath).c_str(),h,txdiag::resource_id("file",h)));
    if (!h) { fclose(f); RET = (uint32_t)FS_STATUS_FATAL_ERROR; return; }
    if (handle_out) rbrew_write32(MEM, handle_out, h);
    s_last_error = FS_STATUS_OK;
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSCloseFile(CPUState* cpu) {
    // r3=client r4=block r5=handle r6=flags r7=async
    uint32_t h = ARG2;
    if (h < 256 && s_file_handles[h]) {
        TXDIAG(txdiag::retire("file",h,0));
        const auto save_path = s_file_writable[h] && s_file_save_slot[h] == 1
            ? s_file_save_path[h] : std::filesystem::path{};
        const int status = fclose(s_file_handles[h]);
        s_file_handles[h] = nullptr; s_file_writable[h] = false; s_file_save_slot[h] = 0;
        s_file_save_path[h].clear();
        const bool controls_ok = status == 0 && (save_path.empty() ||
            rbrew_control_defaults::apply_save_file(save_path) != rbrew_control_defaults::FileResult::Failed);
        RET = controls_ok ? uint32_t(FS_STATUS_OK) : uint32_t(FS_STATUS_ACCESS_ERROR);
        return;
    }
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSReadFile(CPUState* cpu) {
    // r3=client r4=block r5=buf r6=size r7=count r8=handle r9=flags r10=async
    uint32_t buf = ARG2, size = ARG3, count = ARG4, h = ARG5;
    if (h >= 256 || !s_file_handles[h]) { RET = 0; return; }
    require_guest_range(cpu,buf,uint64_t(size)*count);
#ifdef RBREW_TEXTURE_DIAGNOSTICS
    const long diagnostic_offset=ftell(s_file_handles[h]);
#endif
    size_t n = fread(MEM + buf, size, count, s_file_handles[h]);
    TXDIAG(if(uint64_t(n)*size>=4096 || txdiag::sample("asset_read",h))
        txdiag::event("asset_read","\"file_id\":%llu,\"destination\":%u,\"requested\":%llu,\"bytes\":%llu,\"offset\":%ld,\"lr\":%u",txdiag::resource_id("file",h),buf,uint64_t(size)*count,uint64_t(n)*size,diagnostic_offset,cpu->lr));
    RET = (uint32_t)n;
}

static void FSReadFileWithPos(CPUState* cpu) {
    // r3=client r4=block r5=buf r6=size r7=count r8=pos r9=handle r10=flags
    uint32_t buf = ARG2, size = ARG3, count = ARG4, pos = ARG5, h = ARG6;
    if (h >= 256 || !s_file_handles[h]) { RET = 0; return; }
    require_guest_range(cpu,buf,uint64_t(size)*count);
    fseek(s_file_handles[h], (long)pos, SEEK_SET);
    size_t n = fread(MEM + buf, size, count, s_file_handles[h]);
    TXDIAG(if(uint64_t(n)*size>=4096 || txdiag::sample("asset_read",h))
        txdiag::event("asset_read","\"file_id\":%llu,\"destination\":%u,\"requested\":%llu,\"bytes\":%llu,\"offset\":%u,\"lr\":%u",txdiag::resource_id("file",h),buf,uint64_t(size)*count,uint64_t(n)*size,pos,cpu->lr));
    RET = (uint32_t)n;
}

static void FSWriteFile(CPUState* cpu) {
    const uint32_t buf = ARG2, size = ARG3, count = ARG4, h = ARG5;
    if (h >= 256 || !s_file_handles[h]) { RET = uint32_t(FS_STATUS_FATAL_ERROR); return; }
    if (!s_file_writable[h]) { RET = uint32_t(FS_STATUS_PERMISSION_ERROR); return; }
    require_guest_range(cpu, buf, uint64_t(size) * count);
    const size_t written = std::fwrite(MEM + buf, size, count, s_file_handles[h]);
    RET = std::ferror(s_file_handles[h]) ? uint32_t(FS_STATUS_ACCESS_ERROR) : uint32_t(written);
}

static void FSSetPosFile(CPUState* cpu) {
    // r3=client r4=block r5=handle r6=pos r7=flags r8=async
    uint32_t h = ARG2, pos = ARG3;
    if (h < 256 && s_file_handles[h]) fseek(s_file_handles[h], (long)pos, SEEK_SET);
    RET = (uint32_t)FS_STATUS_OK;
}

#ifdef _WIN32
using HostStat = struct _stat64;
#else
using HostStat = struct stat;
#endif

// Cafe FSStat is 0x64 bytes, with a big-endian u32 size at +0x10.
// Host metadata must be serialized rather than cast to a guest structure.
static bool write_fs_stat(CPUState* cpu, uint32_t out, const HostStat& info, bool writable = false) {
    if (!out || !guest_range(cpu,out,0x64)) return false;
#ifdef _WIN32
    bool directory = (info.st_mode & _S_IFDIR) != 0;
#else
    bool directory = S_ISDIR(info.st_mode);
#endif
    if (info.st_size < 0 || uint64_t(info.st_size) > (std::numeric_limits<uint32_t>::max)())
        return false;
    uint32_t size = directory ? 0 : uint32_t(info.st_size);
    std::memset(MEM + out, 0, 0x64);
    rbrew_write32(MEM, out, directory ? 0x80000000u : 0x01000000u);
    // The content namespace is read-only. Directory traversal remains permitted.
    rbrew_write32(MEM, out + 0x04, directory ? (writable ? 0x777u : 0x555u) : (writable ? 0x666u : 0x444u));
    rbrew_write32(MEM, out + 0x10, size);
    rbrew_write32(MEM, out + 0x14, size);
    return true;
}

static void FSGetStatFile(CPUState* cpu) {
    // r3=client r4=block r5=handle r6=stat_out r7=flags r8=async
    uint32_t h = ARG2, stat_out = ARG3;
    if (h >= 256 || !s_file_handles[h]) { RET = (uint32_t)FS_STATUS_FATAL_ERROR; return; }
    HostStat info{};
#ifdef _WIN32
    int result = _fstat64(_fileno(s_file_handles[h]), &info);
#else
    int result = fstat(fileno(s_file_handles[h]), &info);
#endif
    if (result != 0 || !write_fs_stat(cpu, stat_out, info)) {
        RET = (uint32_t)FS_STATUS_FATAL_ERROR; return;
    }
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSGetStat(CPUState* cpu) {
    // r3=client r4=block r5=path r6=stat_out r7=flags r8=async
    char host[512];
    const char* gpath = guest_string(cpu,ARG2);
    uint32_t stat_out = ARG3;
    if (!remap_path(gpath, host, sizeof(host))) { RET = (uint32_t)FS_STATUS_PERMISSION_ERROR; return; }
    HostStat st{};
#ifdef _WIN32
    int result = _stat64(host, &st);
#else
    int result = stat(host, &st);
#endif
    if (result != 0) {
        RBREW_LOGF(stderr, "[FS] FSGetStat failed: %s\n", host);
        s_last_error = FS_STATUS_NOT_FOUND;
        RET = (uint32_t)FS_STATUS_NOT_FOUND;
        return;
    }
    if (!write_fs_stat(cpu, stat_out, st)) {
        RET = (uint32_t)FS_STATUS_FATAL_ERROR; return;
    }
    s_last_error = FS_STATUS_OK;
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSOpenDir(CPUState* cpu) {
    char host[512];
    const char* gpath = guest_string(cpu,ARG2);
    uint32_t handle_out = ARG3;
    if (handle_out) require_guest_range(cpu,handle_out,4);
    if (!remap_path(gpath, host, sizeof(host))) { RET = (uint32_t)FS_STATUS_PERMISSION_ERROR; return; }
    DIR* d = opendir(host);
    if (!d) {
        RBREW_LOGF(stderr, "[FS] FSOpenDir failed: %s\n", host);
        s_last_error = FS_STATUS_NOT_FOUND; RET = (uint32_t)FS_STATUS_NOT_FOUND; return;
    }
    uint32_t h = alloc_dir_handle(d);
    if (!h) { closedir(d); RET = (uint32_t)FS_STATUS_FATAL_ERROR; return; }
    if (handle_out) rbrew_write32(MEM, handle_out, h);
    s_last_error = FS_STATUS_OK;
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSCloseDir(CPUState* cpu) {
    uint32_t h = ARG2;
    if (h < 256 && s_dir_handles[h]) { closedir(s_dir_handles[h]); s_dir_handles[h] = nullptr; }
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSReadDir(CPUState* cpu) {
    // r3=client r4=block r5=handle r6=entry_out r7=flags r8=async
    uint32_t h = ARG2, entry_out = ARG3;
    if (h >= 256 || !s_dir_handles[h]) { RET = (uint32_t)FS_STATUS_FATAL_ERROR; return; }
    if (entry_out) require_guest_range(cpu,entry_out,64+256);
    struct dirent* de = readdir(s_dir_handles[h]);
    if (!de) { s_last_error = FS_STATUS_END; RET = (uint32_t)FS_STATUS_END; return; }
    if (entry_out) {
        // FSDirEntry: stat(64 bytes) then name(256 bytes) — write name at offset 64
        memset(MEM + entry_out, 0, 64 + 256);
        strncpy((char*)(MEM + entry_out + 64), de->d_name, 255);
    }
    s_last_error = FS_STATUS_OK;
    RET = (uint32_t)FS_STATUS_OK;
}

// The native profile currently exposes account slot 1. Keep its existing
// ./save files in place; the common account has its own namespace.
static bool save_root(uint32_t slot, std::filesystem::path& root) {
    if (slot != 1 && slot != 0xFF) return false;
    root = std::filesystem::path("save");
    if (slot == 0xFF) root /= "common";
    return true;
}

static bool save_path(CPUState* cpu, uint32_t slot, uint32_t address,
                      std::filesystem::path& host) {
    std::filesystem::path root;
    if (!save_root(slot, root)) return false;
    std::string name = guest_string(cpu, address);
    std::replace(name.begin(), name.end(), '\\', '/');
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    const std::filesystem::path relative(name);
    if (relative.has_root_name() || relative.is_absolute()) return false;
    for (const auto& part : relative) if (part == "..") return false;
    std::error_code error;
    const auto base = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    host = std::filesystem::weakly_canonical(root / relative, error);
    if (error) return false;
    auto base_part = base.begin(), part = host.begin();
    for (; base_part != base.end(); ++base_part, ++part)
        if (part == host.end() || *part != *base_part) return false;
    return true;
}

void coreinit_save_init(CPUState* cpu) {
    std::lock_guard lock(s_fs_mutex);
    std::error_code error;
    std::filesystem::create_directories("save", error);
    RET = error ? uint32_t(FS_STATUS_ACCESS_ERROR) : uint32_t(FS_STATUS_OK);
}

void coreinit_save_init_dir(CPUState* cpu) {
    std::lock_guard lock(s_fs_mutex);
    std::filesystem::path root;
    if (!save_root(ARG0, root)) { RET = uint32_t(FS_STATUS_NOT_FOUND); return; }
    std::error_code error;
    std::filesystem::create_directories(root, error);
    RET = error ? uint32_t(FS_STATUS_ACCESS_ERROR) : uint32_t(FS_STATUS_OK);
}

void coreinit_save_open_file(CPUState* cpu) {
    std::lock_guard lock(s_fs_mutex);
    const uint32_t slot = ARG2, output = ARG5;
    std::filesystem::path root, path;
    if (!save_root(slot, root)) { RET = uint32_t(FS_STATUS_NOT_FOUND); return; }
    if (!save_path(cpu, slot, ARG3, path)) { RET = uint32_t(FS_STATUS_PERMISSION_ERROR); return; }
    if (!output || !guest_range(cpu, output, 4)) { RET = uint32_t(FS_STATUS_FATAL_ERROR); return; }
    const std::string mode = guest_string(cpu, ARG4);
    // Always use binary host I/O: Cafe byte counts and save checksums must
    // survive CR/LF and 0x1A unchanged on Windows.
    std::string binary;
    bool writable = false;
    for (const auto* permitted : {"r", "rb", "r+", "rb+", "r+b", "w", "wb", "w+", "wb+", "w+b", "a", "ab", "a+", "ab+", "a+b"}) {
        if (mode != permitted) continue;
        writable = mode.front() != 'r' || mode.find('+') != std::string::npos;
        binary = mode.front(); binary += 'b';
        if (mode.find('+') != std::string::npos) binary += '+';
        break;
    }
    if (binary.empty()) { RET = uint32_t(FS_STATUS_PERMISSION_ERROR); return; }
    if (slot == 1 && rbrew_control_defaults::apply_save_file(path) == rbrew_control_defaults::FileResult::Failed) {
        RET = uint32_t(FS_STATUS_ACCESS_ERROR); return;
    }
    if (writable) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) { RET = uint32_t(FS_STATUS_ACCESS_ERROR); return; }
    }
    FILE* file = std::fopen(path.string().c_str(), binary.c_str());
    if (!file) {
        RET = uint32_t(errno == ENOENT ? FS_STATUS_NOT_FOUND : FS_STATUS_ACCESS_ERROR);
        return;
    }
    const uint32_t handle = alloc_file_handle(file, writable, uint8_t(slot), path);
    if (!handle) { std::fclose(file); RET = uint32_t(FS_STATUS_FATAL_ERROR); return; }
    rbrew_write32(MEM, output, handle);
    RBREW_LOGF(stderr, "[nn_save] SAVEOpenFile: opened '%s' (mode=%s) -> FS handle %u\n",
                 path.string().c_str(), binary.c_str(), handle);
    RET = uint32_t(FS_STATUS_OK);
}

void coreinit_save_flush_quota(CPUState* cpu) {
    std::lock_guard lock(s_fs_mutex);
    std::filesystem::path root;
    if (!save_root(ARG2, root)) { RET = uint32_t(FS_STATUS_NOT_FOUND); return; }
    for (unsigned handle = 1; handle < 256; ++handle) {
        if (!s_file_handles[handle] || !s_file_writable[handle] || s_file_save_slot[handle] != ARG2) continue;
        FILE* file = s_file_handles[handle];
        if (std::fflush(file) != 0) { RET = uint32_t(FS_STATUS_ACCESS_ERROR); return; }
#ifdef _WIN32
        const int status = _commit(_fileno(file));
#else
        const int status = fsync(fileno(file));
#endif
        if (status != 0) { RET = uint32_t(FS_STATUS_ACCESS_ERROR); return; }
    }
    RET = uint32_t(FS_STATUS_OK);
}

void coreinit_save_make_dir(CPUState* cpu) {
    std::lock_guard lock(s_fs_mutex);
    std::filesystem::path path;
    if (!save_path(cpu, ARG2, ARG3, path)) { RET = uint32_t(FS_STATUS_PERMISSION_ERROR); return; }
    std::error_code error;
    std::filesystem::create_directories(path, error);
    RET = error ? uint32_t(FS_STATUS_ACCESS_ERROR) : uint32_t(FS_STATUS_OK);
}

void coreinit_save_get_stat(CPUState* cpu) {
    std::lock_guard lock(s_fs_mutex);
    std::filesystem::path path;
    if (!save_path(cpu, ARG2, ARG3, path)) { RET = uint32_t(FS_STATUS_PERMISSION_ERROR); return; }
    HostStat info{};
#ifdef _WIN32
    const int status = _stat64(path.string().c_str(), &info);
#else
    const int status = stat(path.string().c_str(), &info);
#endif
    if (status != 0) { RET = uint32_t(errno == ENOENT ? FS_STATUS_NOT_FOUND : FS_STATUS_ACCESS_ERROR); return; }
    RET = write_fs_stat(cpu, ARG4, info, true) ? uint32_t(FS_STATUS_OK) : uint32_t(FS_STATUS_FATAL_ERROR);
}

template<void (*Function)(CPUState*)> static void locked_fs_call(CPUState* cpu) {
    std::lock_guard lock(s_fs_mutex);
    Function(cpu);
}

#include "coreinit_addrs.h"
void coreinit_fs_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, locked_fs_call<name>)
    REG(FSInit);
    REG(FSShutdown);
    REG(FSAddClient);
    REG(FSDelClient);
    REG(FSInitCmdBlock);
    REG(FSSetCmdPriority);
    REG(FSSetStateChangeNotification);
    REG(FSGetVolumeState);
    REG(FSFlushQuota);
    REG(FSGetLastError);
    REG(FSGetLastErrorCodeForViewer);
    REG(FSGetCwd);
    REG(FSMakeDir);
    REG(FSOpenFile);
    REG(FSCloseFile);
    REG(FSReadFile);
    REG(FSReadFileWithPos);
    REG(FSWriteFile);
    REG(FSSetPosFile);
    REG(FSGetStatFile);
    REG(FSGetStat);
    REG(FSOpenDir);
    REG(FSCloseDir);
    REG(FSReadDir);
#undef REG
}
