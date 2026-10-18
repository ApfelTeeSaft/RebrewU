// Catch-all stubs for remaining Wii U OS modules:
// nn_save, nn_act, nn_olv, nn_boss, nn_nfp, nn_fp, proc_ui, sysapp,
// nsysnet, zlib125, nlibcurl, mic, nn_ac, nn_acp, nn_ndm
//
// nn_save file operations use the shared coreinit FS provider under ./save/.

#include "../os_common.h"
#include <stdio.h>
#include <string.h>
#include <stdexcept>
#include <string>
#include "../coreinit/coreinit_fs.h"
constexpr uint32_t SAVE_OK = 0;

static void SAVEInit(CPUState* cpu) { coreinit_save_init(cpu); }
static void SAVEInitSaveDir(CPUState* cpu) { coreinit_save_init_dir(cpu); }
static void SAVEOpenFile(CPUState* cpu) { coreinit_save_open_file(cpu); }
static void SAVEFlushQuota(CPUState* cpu) { coreinit_save_flush_quota(cpu); }
static void SAVEMakeDir(CPUState* cpu) { coreinit_save_make_dir(cpu); }
static void SAVEGetStat(CPUState* cpu) { coreinit_save_get_stat(cpu); }
static void SAVEOpenDir(CPUState* cpu) { RET = uint32_t(-6); }

// SAVEGetShared*Path(u64 titleId in r3:r4, const char* name r5, char* out r6,
// u32 outLength r7). Formats match Cemu nn_save; the shared volume is served
// by FS from RBREW_MLC_DIR. A name or output outside accessible guest memory,
// or a path that does not fit, fails explicitly (Cemu: FATAL_ERROR + assert).
static void save_shared_path(CPUState* cpu, const char* format) {
    const uint32_t name = ARG2, out = ARG3, capacity = ARG4;
    std::string file;
    for (uint32_t at = name;; ++at) {
        if (!name || !rbrew_memory_accessible(MEM, at, 1))
            throw std::runtime_error("SAVE shared data name is outside accessible guest memory");
        const uint8_t c = rbrew_read8(MEM, at);
        if (!c) break;
        if (file.size() >= 0x200) throw std::runtime_error("SAVE shared data name is unterminated");
        file.push_back(char(c));
    }
    char path[0x300];
    const int written = snprintf(path, sizeof(path), format, ARG0, ARG1, file.c_str());
    if (written < 0 || uint32_t(written) >= capacity || size_t(written) >= sizeof(path))
        throw std::runtime_error("SAVE shared path does not fit the guest output buffer");
    if (!out || !rbrew_memory_accessible(MEM, out, uint64_t(written) + 1))
        throw std::runtime_error("SAVE shared path output is outside accessible guest memory");
    for (int i = 0; i <= written; ++i) rbrew_write8(MEM, out + uint32_t(i), uint8_t(path[i]));
    RET = SAVE_OK;
}
static void SAVEGetSharedDataTitlePath(CPUState* cpu) {
    save_shared_path(cpu, "/vol/storage_mlc01/sys/title/%08x/%08x/content/%s");
}
static void SAVEGetSharedSaveDataPath(CPUState* cpu) {
    save_shared_path(cpu, "/vol/storage_mlc01/usr/save/%08x/%08x/user/common/%s");
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

// nn::boss::DataName::DataName() - nn_boss.rpl 02027700: a null this is
// allocated from the nn_boss allocator (0x20 bytes); the 32-byte name is
// zeroed and this returned. Gambit constructs arrays of them (0279404C).
static void BOSSDataNameCtor(CPUState* cpu) {
    const uint32_t self = ARG0;
    if (!self)
        throw std::runtime_error("nn::boss::DataName::DataName(nullptr): nn_boss allocator not implemented");
    if (!rbrew_memory_accessible(MEM, self, 0x20))
        throw std::runtime_error("nn::boss::DataName::DataName: object outside guest memory");
    memset(MEM + self, 0, 0x20);
    RET = self;
}

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

// nn::fp::LoginAsync(callback, userParam) - nn_fp.rpl 020019dc forwards the
// request to the friends service (FPD), which later completes it by calling
// callback(const nn::Result*, userParam) (PPC EABI passes the Result by
// pointer). Without an online account the service completes at once with
// success (Cemu iosu_fpd CallHandler_LoginAsync: Splatoon softlocks
// otherwise). Gambit's callback 02A7B590 stores the result and signals the
// event its loader waits on; completing before returning has that effect.
static void FPLoginAsync(CPUState* cpu) {
    const uint32_t callback = ARG0, user = ARG1;
    if (callback) {
        CPUState call = *cpu;
        call.r[1] = (cpu->r[1] - 0x40) & ~0xFu;       // frame below the caller's
        const uint32_t result = call.r[1] + 0x10;
        rbrew_write32(MEM, call.r[1], cpu->r[1]);     // back chain
        rbrew_write32(MEM, result, 0);                // nn::Result: success
        call.r[3] = result; call.r[4] = user;
        rbrew_dispatch(&call, callback);
    }
    RET = 0;
}
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
    rbrew_register_func(cpu, ADDR_LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv, FPLoginAsync);
    rbrew_register_func(cpu, ADDR___ct__Q3_2nn4boss8DataNameFv, BOSSDataNameCtor);
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
