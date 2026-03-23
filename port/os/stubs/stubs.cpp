// Catch-all stubs for remaining Wii U OS modules:
// nn_save, nn_act, nn_olv, nn_boss, nn_nfp, nn_fp, proc_ui, sysapp,
// nsysnet, zlib125, nlibcurl, mic, nn_ac, nn_acp, nn_ndm
//
// nn_save is fully implemented: save files are stored under ./save/

#include "../os_common.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define PATH_SEP '\\'
// Recursive mkdir helper (Windows)
static int mkdirs(const char* path) {
    char buf[512];
    strncpy(buf, path, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
    for (char* p = buf+1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char c = *p; *p = '\0';
            _mkdir(buf);
            *p = c;
        }
    }
    return _mkdir(buf);
}
#else
#  include <unistd.h>
#  define PATH_SEP '/'
static int mkdirs(const char* path) {
    char buf[512];
    strncpy(buf, path, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
    for (char* p = buf+1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    return mkdir(buf, 0755);
}
#endif

// FS status codes (same as coreinit_fs.cpp)
#define SAVE_OK              0
#define SAVE_NOT_FOUND      ((uint32_t)-6)
#define SAVE_ACCESS_ERROR   ((uint32_t)-16)

// ---------------------------------------------------------------------------
// Save file handle table (handles 1..127, distinct from FS handles)
// ---------------------------------------------------------------------------
static FILE* s_save_handles[128] = {};

static uint32_t save_alloc_handle(FILE* f) {
    for (int i = 1; i < 128; i++)
        if (!s_save_handles[i]) { s_save_handles[i] = f; return (uint32_t)i; }
    return 0;
}

// Build a host path for a save-data file.
// Wii U save paths look like "/user/common/cache.dat" or just "savefile.bin".
// We map them to ./save/<path>.
static void save_host_path(char* out, size_t outsz,
                            uint32_t path_guest, const uint8_t* mem) {
    // Read the guest path string
    char gpath[256]; gpath[0] = '\0';
    for (int i = 0; i < 255; i++) {
        char c = (char)mem[path_guest + i];
        gpath[i] = c;
        if (!c) break;
    }
    gpath[255] = '\0';
    // Strip any leading /
    const char* p = gpath;
    while (*p == '/' || *p == '\\') p++;
    // Replace remaining '/' with host path separator
    char fixed[256];
    for (int i = 0; i < 255; i++) {
        char c = p[i];
        if (!c) { fixed[i] = '\0'; break; }
        fixed[i] = (c == '/' || c == '\\') ? PATH_SEP : c;
    }
    fixed[255] = '\0';
    snprintf(out, outsz, "save%c%s", PATH_SEP, fixed);
}

static void SAVEInit(CPUState* cpu) {
    mkdirs("save");   // ensure save dir exists
    RET = SAVE_OK;
}

static void SAVEInitSaveDir(CPUState* cpu) {
    mkdirs("save");
    RET = SAVE_OK;
}

// SAVEOpenFile(client, block, accountSlot, path_guest, mode_guest, handle_out_guest, errorMask)
// r3=client r4=block r5=slot r6=path r7=mode r8=handle_out r9=errorMask
static void SAVEOpenFile(CPUState* cpu) {
    uint32_t path_addr   = ARG3;    // r6
    uint32_t mode_addr   = ARG4;    // r7
    uint32_t handle_out  = ARG5;    // r8

    if (!path_addr) { RET = SAVE_NOT_FOUND; return; }

    char host_path[512];
    save_host_path(host_path, sizeof(host_path), path_addr, MEM);

    // Read mode string from guest
    char mode[8] = "rb";
    if (mode_addr) {
        for (int i = 0; i < 7; i++) {
            mode[i] = (char)MEM[mode_addr + i];
            if (!mode[i]) break;
        }
        mode[7] = '\0';
    }

    // Ensure parent directory exists
    {
        char parent[512];
        strncpy(parent, host_path, sizeof(parent)-1);
        parent[sizeof(parent)-1] = '\0';
        char* last_sep = nullptr;
        for (char* p = parent; *p; p++)
            if (*p == PATH_SEP) last_sep = p;
        if (last_sep) { *last_sep = '\0'; mkdirs(parent); }
    }

    FILE* f = fopen(host_path, mode);
    if (!f) {
        // If opened for writing and file doesn't exist, try creating it
        if (mode[0] == 'w' || mode[0] == 'a') {
            f = fopen(host_path, "wb");
        }
        if (!f) {
            fprintf(stderr, "[nn_save] SAVEOpenFile: cannot open '%s' (mode=%s)\n",
                    host_path, mode);
            if (handle_out) rbrew_write32(MEM, handle_out, (uint32_t)-1);
            RET = SAVE_NOT_FOUND;
            return;
        }
        if (mode[0] != 'w') { fclose(f); f = fopen(host_path, mode); }
        if (!f) { RET = SAVE_NOT_FOUND; return; }
    }

    uint32_t h = save_alloc_handle(f);
    if (!h) { fclose(f); RET = SAVE_ACCESS_ERROR; return; }

    if (handle_out) rbrew_write32(MEM, handle_out, h);
    fprintf(stderr, "[nn_save] SAVEOpenFile: opened '%s' (mode=%s) → handle %u\n",
            host_path, mode, h);
    RET = SAVE_OK;
}

// SAVEReadFile(client, block, buf_guest, size, count, handle, flags, errorMask)
// r3=client r4=block r5=buf r6=size r7=count r8=handle r9=flags r10=errorMask
static void SAVEReadFile(CPUState* cpu) {
    uint32_t buf_addr = ARG2;   // r5
    uint32_t elem_sz  = ARG3;   // r6
    uint32_t count    = ARG4;   // r7
    uint32_t handle   = ARG5;   // r8

    if (handle == 0 || handle >= 128 || !s_save_handles[handle]) { RET = 0; return; }
    if (!buf_addr || !elem_sz || !count) { RET = 0; return; }

    size_t n = fread(MEM + buf_addr, elem_sz, count, s_save_handles[handle]);
    RET = (uint32_t)n;
}

// SAVEWriteFile(client, block, buf_guest, size, count, handle, flags, errorMask)
// r3=client r4=block r5=buf r6=size r7=count r8=handle r9=flags r10=errorMask
static void SAVEWriteFile(CPUState* cpu) {
    uint32_t buf_addr = ARG2;   // r5
    uint32_t elem_sz  = ARG3;   // r6
    uint32_t count    = ARG4;   // r7
    uint32_t handle   = ARG5;   // r8

    if (handle == 0 || handle >= 128 || !s_save_handles[handle]) { RET = 0; return; }
    if (!buf_addr || !elem_sz || !count) { RET = 0; return; }

    size_t n = fwrite(MEM + buf_addr, elem_sz, count, s_save_handles[handle]);
    RET = (uint32_t)n;
}

// SAVECloseFile(client, block, handle, errorMask)
// r3=client r4=block r5=handle r6=errorMask
static void SAVECloseFile(CPUState* cpu) {
    uint32_t handle = ARG2;   // r5
    if (handle > 0 && handle < 128 && s_save_handles[handle]) {
        fclose(s_save_handles[handle]);
        s_save_handles[handle] = nullptr;
    }
    RET = SAVE_OK;
}

static void SAVEFlushQuota(CPUState* cpu) { RET = SAVE_OK; }

// SAVEMakeDir(client, block, accountSlot, path_guest, errorMask)
// r3=client r4=block r5=slot r6=path r7=errorMask
static void SAVEMakeDir(CPUState* cpu) {
    uint32_t path_addr = ARG3;   // r6
    if (!path_addr) { RET = SAVE_OK; return; }
    char host_path[512];
    save_host_path(host_path, sizeof(host_path), path_addr, MEM);
    mkdirs(host_path);
    RET = SAVE_OK;
}

// SAVEGetStat(client, block, accountSlot, path_guest, stat_out, errorMask)
// r3=client r4=block r5=slot r6=path r7=stat_out r8=errorMask
// FSStat layout (big-endian, relevant fields only):
//   +0x00 uint32 flags       (bitfield: 0x80000000 = is directory, 0x01000000 = file)
//   +0x04 uint32 permissions
//   +0x10 uint32 size        (file size in bytes)
static void SAVEGetStat(CPUState* cpu) {
    uint32_t path_addr = ARG3;   // r6
    uint32_t stat_out  = ARG4;   // r7

    if (!path_addr) { RET = SAVE_NOT_FOUND; return; }

    char host_path[512];
    save_host_path(host_path, sizeof(host_path), path_addr, MEM);

    struct stat st;
    if (::stat(host_path, &st) != 0) {
        RET = SAVE_NOT_FOUND;
        return;
    }

    if (stat_out) {
        // Zero the stat structure (0x64 bytes)
        for (int i = 0; i < 0x64; i += 4)
            rbrew_write32(MEM, stat_out + (uint32_t)i, 0u);

        uint32_t flags = 0;
#ifdef S_ISDIR
        if (S_ISDIR(st.st_mode)) flags = 0x80000000u;
        else flags = 0x01000000u;
#endif
        rbrew_write32(MEM, stat_out + 0x00u, flags);
        rbrew_write32(MEM, stat_out + 0x04u, 0x000001FFu);  // rwxrwxrwx
        rbrew_write32(MEM, stat_out + 0x10u, (uint32_t)st.st_size);
    }
    RET = SAVE_OK;
}

static void SAVEOpenDir(CPUState* cpu)  { RET = SAVE_NOT_FOUND; }
static void SAVECloseDir(CPUState* cpu) { RET = SAVE_OK; }
static void SAVEReadDir(CPUState* cpu)  { RET = SAVE_NOT_FOUND; }

static void SAVEGetSharedDataTitlePath(CPUState* cpu) {
    if (ARG2) rbrew_write8(MEM, ARG2, 0);
    RET = SAVE_OK;
}
static void SAVEGetSharedSaveDataPath(CPUState* cpu) {
    if (ARG2) rbrew_write8(MEM, ARG2, 0);
    RET = SAVE_OK;
}

static void SAVEGetFreeSpaceSize(CPUState* cpu) {
    // Report 1 GB free (r3/r4 = high/low uint64)
    cpu->r[3] = 0;
    cpu->r[4] = 0x40000000u;
    RET = SAVE_OK;
}

// ---------------------------------------------------------------------------
// nn_act — account information (return dummy data)
// ---------------------------------------------------------------------------

static void ACTInit(CPUState* cpu)               { RET = 0; }
static void ACTGetSlotNo(CPUState* cpu)          { RET = 1; } // slot 1
static void ACTGetDefaultAccount(CPUState* cpu)  { RET = 1; }
static void ACTGetMii(CPUState* cpu)             { RET = (uint32_t)-1; }
static void ACTGetAccountId(CPUState* cpu) {
    // Write a dummy account ID string
    if (ARG0) {
        static const char id[] = "GambitPortUser";
        for (size_t i = 0; i <= sizeof(id); ++i)
            rbrew_write8(MEM, ARG0 + (uint32_t)i, (uint8_t)id[i]);
    }
    RET = 0;
}
static void ACTGetPrincipalId(CPUState* cpu) {
    // r3 = slot, r4 = principalId*
    if (ARG1) rbrew_write32(MEM, ARG1, 0x12345678u);
    RET = 0;
}
static void ACTGetPersistentId(CPUState* cpu)   { RET = 0xABCD0001u; }
static void ACTIsNetworkAccount(CPUState* cpu)  { RET = 0; }
static void ACTIsPasswordCacheEnabled(CPUState* cpu) { RET = 0; }
static void ACTGetCountryName(CPUState* cpu)    { RET = 0; }
static void ACTGetTimeZoneId(CPUState* cpu)     { RET = 0; }
static void ACTFinalizeLibrary(CPUState* cpu)   { (void)cpu; }

// ---------------------------------------------------------------------------
// nn_olv — Miiverse (always unavailable)
// ---------------------------------------------------------------------------

static void OLVInit(CPUState* cpu)              { RET = (uint32_t)-2146435072u; } // NN_RESULT_FAILED
static void OLVCalculatePostListParam(CPUState* cpu) { RET = (uint32_t)-2146435072u; }
static void OLVDownloadPostListParam(CPUState* cpu)  { RET = (uint32_t)-2146435072u; }
static void OLVGetIsInitialized(CPUState* cpu)  { RET = 0; }

// ---------------------------------------------------------------------------
// nn_boss — SpotPass (always unavailable)
// ---------------------------------------------------------------------------

static void BOSSInit(CPUState* cpu)             { RET = (uint32_t)-1; }
static void BOSSGetStorageInfo(CPUState* cpu)   { RET = (uint32_t)-1; }
static void BOSSGetTaskIdList(CPUState* cpu)    { RET = (uint32_t)-1; }

// ---------------------------------------------------------------------------
// nn_nfp — Amiibo (not present)
// ---------------------------------------------------------------------------

static void NFPInit(CPUState* cpu)              { RET = 0; }
static void NFPInitialize(CPUState* cpu)        { RET = 0; }
static void NFPGetNfpState(CPUState* cpu)       { RET = 0; } // NFP_STATE_INIT
static void NFPStartDetection(CPUState* cpu)    { RET = 0; }
static void NFPStopDetection(CPUState* cpu)     { RET = 0; }
static void NFPMount(CPUState* cpu)             { RET = (uint32_t)-1; }
static void NFPIsNfcEnabled(CPUState* cpu)      { RET = 0; }
static void NFPFinalize(CPUState* cpu)          { RET = 0; }

// ---------------------------------------------------------------------------
// nn_fp — friend list (empty)
// ---------------------------------------------------------------------------

static void FPInit(CPUState* cpu)               { RET = 0; }
static void FPIsInitialized(CPUState* cpu)      { RET = 1; }
static void FPGetFriendList(CPUState* cpu) {
    if (ARG2) rbrew_write32(MEM, ARG2, 0u); // 0 friends
    RET = 0;
}
static void FPGetFriendCount(CPUState* cpu)     { RET = 0; }
static void FPIsOnline(CPUState* cpu)           { RET = 0; }
static void FPFinalize(CPUState* cpu)           { (void)cpu; }

// ---------------------------------------------------------------------------
// proc_ui — Process UI (foreground/background transitions)
// ---------------------------------------------------------------------------

static void ProcUIInit(CPUState* cpu)            { (void)cpu; }
static void ProcUIInitEx(CPUState* cpu)          { (void)cpu; }
static void ProcUIShutdown(CPUState* cpu)        { (void)cpu; }
// ProcUIProcessMessages(blocking) → PROCUI_STATUS_IN_FOREGROUND (0)
static void ProcUIProcessMessages(CPUState* cpu) { RET = 0; }
// ProcUISubProcessMessages(blocking) → 0
static void ProcUISubProcessMessages(CPUState* cpu) { RET = 0; }
static void ProcUISetMEM1Storage(CPUState* cpu)  { (void)cpu; }
static void ProcUISetBucketStorage(CPUState* cpu){ (void)cpu; }
static void ProcUIInForeground(CPUState* cpu)    { RET = 1; }
static void ProcUIIsRunning(CPUState* cpu)       { RET = 1; }
static void ProcUIRegisterCallback(CPUState* cpu){ (void)cpu; }
static void ProcUIRegisterCallbackCore(CPUState* cpu) { (void)cpu; }
static void ProcUIClearCallbacks(CPUState* cpu)  { (void)cpu; }

// ---------------------------------------------------------------------------
// sysapp — system application helpers
// ---------------------------------------------------------------------------

static void SYSRelaunchTitle(CPUState* cpu)     { (void)cpu; }
static void SYSGetTitleLoc(CPUState* cpu) {
    // Return empty path
    if (ARG0) rbrew_write8(MEM, ARG0, 0);
}
static void SYSLaunchTitle(CPUState* cpu)       { (void)cpu; }
static void SYSLaunchMenu(CPUState* cpu)        { (void)cpu; }
static void SYSCheckTitleExists(CPUState* cpu)  { RET = 0; }

// ---------------------------------------------------------------------------
// nsysnet — network (return no-network)
// ---------------------------------------------------------------------------

static void NSYSNETInit(CPUState* cpu)          { RET = 0; }
static void socket_(CPUState* cpu)              { RET = (uint32_t)-1; }
static void bind_(CPUState* cpu)                { RET = (uint32_t)-1; }
static void connect_(CPUState* cpu)             { RET = (uint32_t)-1; }
static void send_(CPUState* cpu)                { RET = (uint32_t)-1; }
static void recv_(CPUState* cpu)                { RET = (uint32_t)-1; }
static void closesocket_(CPUState* cpu)         { RET = 0; }
static void setsockopt_(CPUState* cpu)          { RET = (uint32_t)-1; }
static void getsockopt_(CPUState* cpu)          { RET = (uint32_t)-1; }
static void getaddrinfo_(CPUState* cpu)         { RET = (uint32_t)-1; }
static void freeaddrinfo_(CPUState* cpu)        { (void)cpu; }
static void inet_ntop_(CPUState* cpu)           { RET = 0; }
static void inet_pton_(CPUState* cpu)           { RET = 0; }

// ---------------------------------------------------------------------------
// zlib125 — pass through to host zlib
// Standard zlib API: compress(dst, dstLen*, src, srcLen)
// Both dst and src are guest addresses; dstLen* is also a guest pointer.
// ---------------------------------------------------------------------------
#include <zlib.h>

static void zlib_compress(CPUState* cpu) {
    // r3=dst, r4=*dstLen (in/out), r5=src, r6=srcLen
    uint32_t dst_addr    = ARG0;
    uint32_t dst_len_ptr = ARG1;
    uint32_t src_addr    = ARG2;
    uLong    src_len     = ARG3;
    uLongf   dst_len     = (dst_len_ptr ? rbrew_read32(MEM, dst_len_ptr) : 0u);
    int ret = compress(MEM + dst_addr, &dst_len, MEM + src_addr, src_len);
    if (dst_len_ptr) rbrew_write32(MEM, dst_len_ptr, (uint32_t)dst_len);
    RET = (uint32_t)ret;
}

static void zlib_uncompress(CPUState* cpu) {
    // r3=dst, r4=*dstLen (in/out), r5=src, r6=srcLen
    uint32_t dst_addr    = ARG0;
    uint32_t dst_len_ptr = ARG1;
    uint32_t src_addr    = ARG2;
    uLong    src_len     = ARG3;
    uLongf   dst_len     = (dst_len_ptr ? rbrew_read32(MEM, dst_len_ptr) : 0u);
    int ret = uncompress(MEM + dst_addr, &dst_len, MEM + src_addr, src_len);
    if (dst_len_ptr) rbrew_write32(MEM, dst_len_ptr, (uint32_t)dst_len);
    RET = (uint32_t)ret;
}

// zlib CRC32: crc32(crc, buf_guest_addr, len)
static void zlib_crc32(CPUState* cpu) {
    uLong    crc = (uLong)ARG0;
    uint32_t buf = ARG1;
    uInt     len = (uInt)ARG2;
    RET = (uint32_t)crc32(crc, MEM + buf, len);
}

// mic — microphone stubs
static void MICInit(CPUState* cpu)    { RET = 0; }
static void MICOpen(CPUState* cpu)    { RET = (uint32_t)-1; }
static void MICClose(CPUState* cpu)   { (void)cpu; }

// nn_ac (network settings) stubs
static void ACInitialize(CPUState* cpu)   { RET = 0; }
static void ACConnect(CPUState* cpu)      { RET = 0; }
static void ACIsApplicationConnected(CPUState* cpu) { RET = 0; }
static void ACGetStatus(CPUState* cpu)    { RET = 0; }

// nn_acp stubs
static void ACPCheckTitleLaunchByTitleListType(CPUState* cpu) { RET = 0; }
static void ACPGetTitleMetaXml(CPUState* cpu)                 { RET = (uint32_t)-1; }

// nn_ndm stubs
static void NDMEnterExclusiveMode(CPUState* cpu) { RET = 0; }
static void NDMExitExclusiveMode(CPUState* cpu)  { RET = 0; }
static void NDMGetCurrentState(CPUState* cpu)    { RET = 0; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "stubs_addrs.h"

void stubs_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    // nn_save
    REG(SAVEInit);
    REG(SAVEInitSaveDir);
    REG(SAVEGetSharedDataTitlePath);
    REG(SAVEGetSharedSaveDataPath);
    REG(SAVEOpenFile);
    // REG(SAVECloseFile); // not in this game's PLT
    // REG(SAVEReadFile); // not in this game's PLT
    // REG(SAVEWriteFile); // not in this game's PLT
    REG(SAVEFlushQuota);
    REG(SAVEMakeDir);
    REG(SAVEOpenDir);
    // REG(SAVECloseDir); // not in this game's PLT
    // REG(SAVEReadDir); // not in this game's PLT
    REG(SAVEGetStat);
    // REG(SAVEGetFreeSpaceSize); // not in this game's PLT
    // nn_act
    // REG(ACTInit); // not in this game's PLT
    // REG(ACTGetSlotNo); // not in this game's PLT
    // REG(ACTGetDefaultAccount); // not in this game's PLT
    // REG(ACTGetMii); // not in this game's PLT
    // REG(ACTGetAccountId); // not in this game's PLT
    // REG(ACTGetPrincipalId); // not in this game's PLT
    // REG(ACTGetPersistentId); // not in this game's PLT
    // REG(ACTIsNetworkAccount); // not in this game's PLT
    // REG(ACTIsPasswordCacheEnabled); // not in this game's PLT
    // REG(ACTGetCountryName); // not in this game's PLT
    // REG(ACTGetTimeZoneId); // not in this game's PLT
    // REG(ACTFinalizeLibrary); // not in this game's PLT
    // nn_olv
    // REG(OLVInit); // not in this game's PLT
    // REG(OLVCalculatePostListParam); // not in this game's PLT
    // REG(OLVDownloadPostListParam); // not in this game's PLT
    // REG(OLVGetIsInitialized); // not in this game's PLT
    // nn_boss
    // REG(BOSSInit); // not in this game's PLT
    // REG(BOSSGetStorageInfo); // not in this game's PLT
    // REG(BOSSGetTaskIdList); // not in this game's PLT
    // nn_nfp
    // REG(NFPInit); // not in this game's PLT
    // REG(NFPInitialize); // not in this game's PLT
    // REG(NFPGetNfpState); // not in this game's PLT
    // REG(NFPStartDetection); // not in this game's PLT
    // REG(NFPStopDetection); // not in this game's PLT
    // REG(NFPMount); // not in this game's PLT
    // REG(NFPIsNfcEnabled); // not in this game's PLT
    // REG(NFPFinalize); // not in this game's PLT
    // nn_fp
    // REG(FPInit); // not in this game's PLT
    // REG(FPIsInitialized); // not in this game's PLT
    // REG(FPGetFriendList); // not in this game's PLT
    // REG(FPGetFriendCount); // not in this game's PLT
    // REG(FPIsOnline); // not in this game's PLT
    // REG(FPFinalize); // not in this game's PLT
    // proc_ui
    REG(ProcUIInit);
    // REG(ProcUIInitEx); // not in this game's PLT
    REG(ProcUIShutdown);
    REG(ProcUIProcessMessages);
    // REG(ProcUISubProcessMessages); // not in this game's PLT
    // REG(ProcUISetMEM1Storage); // not in this game's PLT
    // REG(ProcUISetBucketStorage); // not in this game's PLT
    // REG(ProcUIInForeground); // not in this game's PLT
    // REG(ProcUIIsRunning); // not in this game's PLT
    REG(ProcUIRegisterCallback);
    // REG(ProcUIRegisterCallbackCore); // not in this game's PLT
    // REG(ProcUIClearCallbacks); // not in this game's PLT
    // sysapp
    REG(SYSRelaunchTitle);
    // REG(SYSGetTitleLoc); // not in this game's PLT
    // REG(SYSLaunchTitle); // not in this game's PLT
    REG(SYSLaunchMenu);
    // REG(SYSCheckTitleExists); // not in this game's PLT
    // nsysnet
    // REG(NSYSNETInit); // not in this game's PLT
    // REG(socket_); // not in this game's PLT
    // REG(bind_); // not in this game's PLT
    // REG(connect_); // not in this game's PLT
    // REG(send_); // not in this game's PLT
    // REG(recv_); // not in this game's PLT
    // REG(closesocket_); // not in this game's PLT
    // REG(setsockopt_); // not in this game's PLT
    // REG(getsockopt_); // not in this game's PLT
    // REG(getaddrinfo_); // not in this game's PLT
    // REG(freeaddrinfo_); // not in this game's PLT
    // REG(inet_ntop_); // not in this game's PLT
    // REG(inet_pton_); // not in this game's PLT
    // zlib125
    // REG(zlib_compress); // not in this game's PLT
    // REG(zlib_uncompress); // not in this game's PLT
    // REG(zlib_crc32); // not in this game's PLT
    // mic
    REG(MICInit);
    REG(MICOpen);
    REG(MICClose);
    // nn_ac
    // REG(ACInitialize); // not in this game's PLT
    // REG(ACConnect); // not in this game's PLT
    // REG(ACIsApplicationConnected); // not in this game's PLT
    // REG(ACGetStatus); // not in this game's PLT
    // nn_acp
    // REG(ACPCheckTitleLaunchByTitleListType); // not in this game's PLT
    // REG(ACPGetTitleMetaXml); // not in this game's PLT
    // nn_ndm
    // REG(NDMEnterExclusiveMode); // not in this game's PLT
    // REG(NDMExitExclusiveMode); // not in this game's PLT
    // REG(NDMGetCurrentState); // not in this game's PLT
#undef REG
}
