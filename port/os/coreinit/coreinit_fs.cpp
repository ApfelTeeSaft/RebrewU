// coreinit filesystem: FSInit, FSOpenFile, FSReadFile, etc. — host-backed I/O
#include "../os_common.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#ifdef _WIN32
#  include "../../dirent_win.h"
#  include <direct.h>
#  define mkdir(p,m) _mkdir(p)
#else
#  include <dirent.h>
#endif
#include <unordered_map>
#include <mutex>

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
static DIR*             s_dir_handles[256]  = {};
static int32_t          s_last_error        = FS_STATUS_OK;

// Map guest content path prefix to host path
static const char* remap_path(const char* guest, char* out, size_t outsz) {
    // Strip Wii U content:/ or vol/content/ prefix
    if (strncmp(guest, "/vol/content", 12) == 0) guest += 12;
    else if (strncmp(guest, "content:", 8) == 0)  guest += 8;
    // Build host path relative to current working directory
    snprintf(out, outsz, ".%s", guest);
    return out;
}

static uint32_t alloc_file_handle(FILE* f) {
    for (int i = 1; i < 256; i++) if (!s_file_handles[i]) { s_file_handles[i] = f; return (uint32_t)i; }
    return 0;
}
static uint32_t alloc_dir_handle(DIR* d) {
    for (int i = 1; i < 256; i++) if (!s_dir_handles[i]) { s_dir_handles[i] = d; return (uint32_t)i; }
    return 0;
}

static void FSInit(CPUState* cpu) { (void)cpu; s_last_error = FS_STATUS_OK; }
static void FSShutdown(CPUState* cpu) { (void)cpu; }
static void FSAddClient(CPUState* cpu) { RET = (uint32_t)FS_STATUS_OK; }
static void FSDelClient(CPUState* cpu) { RET = (uint32_t)FS_STATUS_OK; }
static void FSInitCmdBlock(CPUState* cpu) {
    // Zero the FSCmdBlock pointed to by r3
    if (ARG0) memset(MEM + ARG0, 0, 0xA80);
}
static void FSSetCmdPriority(CPUState* cpu) { (void)cpu; }
static void FSSetStateChangeNotification(CPUState* cpu) { (void)cpu; }
static void FSGetVolumeState(CPUState* cpu) { RET = 0; } // FS_VOLUME_STATE_READY
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
        strncpy((char*)(MEM + buf), cwd, len);
        MEM[buf + len - 1] = '\0';
    }
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSMakeDir(CPUState* cpu) {
    char host[512];
    const char* gpath = (char*)(MEM + ARG2);
    remap_path(gpath, host, sizeof(host));
    mkdir(host, 0755);
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSOpenFile(CPUState* cpu) {
    // r3=client r4=block r5=path r6=mode r7=handle_out r8=flags r9=async
    char host[512];
    const char* gpath = (char*)(MEM + ARG2);
    const char* mode  = (char*)(MEM + ARG3);
    uint32_t handle_out = ARG4;
    remap_path(gpath, host, sizeof(host));
    FILE* f = fopen(host, mode);
    if (!f) {
        s_last_error = FS_STATUS_NOT_FOUND;
        RET = (uint32_t)FS_STATUS_NOT_FOUND;
        return;
    }
    uint32_t h = alloc_file_handle(f);
    if (!h) { fclose(f); RET = (uint32_t)FS_STATUS_FATAL_ERROR; return; }
    if (handle_out) rbrew_write32(MEM, handle_out, h);
    s_last_error = FS_STATUS_OK;
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSCloseFile(CPUState* cpu) {
    // r3=client r4=block r5=handle r6=flags r7=async
    uint32_t h = ARG2;
    if (h < 256 && s_file_handles[h]) { fclose(s_file_handles[h]); s_file_handles[h] = nullptr; }
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSReadFile(CPUState* cpu) {
    // r3=client r4=block r5=buf r6=size r7=count r8=handle r9=flags r10=async
    uint32_t buf = ARG2, size = ARG3, count = ARG4, h = ARG5;
    if (h >= 256 || !s_file_handles[h]) { RET = 0; return; }
    size_t n = fread(MEM + buf, size, count, s_file_handles[h]);
    RET = (uint32_t)n;
}

static void FSReadFileWithPos(CPUState* cpu) {
    // r3=client r4=block r5=buf r6=size r7=count r8=pos r9=handle r10=flags
    uint32_t buf = ARG2, size = ARG3, count = ARG4, pos = ARG5, h = ARG6;
    if (h >= 256 || !s_file_handles[h]) { RET = 0; return; }
    fseek(s_file_handles[h], (long)pos, SEEK_SET);
    size_t n = fread(MEM + buf, size, count, s_file_handles[h]);
    RET = (uint32_t)n;
}

static void FSWriteFile(CPUState* cpu) {
    // r3=client r4=block r5=buf r6=size r7=count r8=handle r9=flags r10=async
    uint32_t buf = ARG2, size = ARG3, count = ARG4, h = ARG5;
    if (h >= 256 || !s_file_handles[h]) { RET = 0; return; }
    size_t n = fwrite(MEM + buf, size, count, s_file_handles[h]);
    RET = (uint32_t)n;
}

static void FSSetPosFile(CPUState* cpu) {
    // r3=client r4=block r5=handle r6=pos r7=flags r8=async
    uint32_t h = ARG2, pos = ARG3;
    if (h < 256 && s_file_handles[h]) fseek(s_file_handles[h], (long)pos, SEEK_SET);
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSGetStatFile(CPUState* cpu) {
    // r3=client r4=block r5=handle r6=stat_out r7=flags r8=async
    uint32_t h = ARG2, stat_out = ARG3;
    if (h >= 256 || !s_file_handles[h]) { RET = (uint32_t)FS_STATUS_FATAL_ERROR; return; }
    long cur = ftell(s_file_handles[h]);
    fseek(s_file_handles[h], 0, SEEK_END);
    long sz = ftell(s_file_handles[h]);
    fseek(s_file_handles[h], cur, SEEK_SET);
    if (stat_out) {
        // FSStatSize is at offset 0 in FSStat (big-endian uint64 at offset 0)
        rbrew_write32(MEM, stat_out, 0);       // size hi
        rbrew_write32(MEM, stat_out+4, (uint32_t)sz); // size lo
    }
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSGetStat(CPUState* cpu) {
    // r3=client r4=block r5=path r6=stat_out r7=flags r8=async
    char host[512];
    const char* gpath = (char*)(MEM + ARG2);
    uint32_t stat_out = ARG3;
    remap_path(gpath, host, sizeof(host));
    struct stat st;
    if (stat(host, &st) != 0) {
        s_last_error = FS_STATUS_NOT_FOUND;
        RET = (uint32_t)FS_STATUS_NOT_FOUND;
        return;
    }
    if (stat_out) {
        rbrew_write32(MEM, stat_out,   0);
        rbrew_write32(MEM, stat_out+4, (uint32_t)st.st_size);
    }
    s_last_error = FS_STATUS_OK;
    RET = (uint32_t)FS_STATUS_OK;
}

static void FSOpenDir(CPUState* cpu) {
    char host[512];
    const char* gpath = (char*)(MEM + ARG2);
    uint32_t handle_out = ARG3;
    remap_path(gpath, host, sizeof(host));
    DIR* d = opendir(host);
    if (!d) { s_last_error = FS_STATUS_NOT_FOUND; RET = (uint32_t)FS_STATUS_NOT_FOUND; return; }
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

#include "coreinit_addrs.h"
void coreinit_fs_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
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
