#include "nn_act.h"
#include "../stubs/stubs_addrs.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>

// Evidence: Gambit FUN_027D626C (account/Mii owner, 0x9A4 bytes) calls
// Initialize (sign bit -> assert line 0x3D), GetSlotNo, IsNetworkAccount,
// GetMiiName, GetMii, then FUN_027F147C -> FFL FUN_02C6DB64(out, 4, store, 0, 0)
// whose nonzero result asserts at line 0x59. It then reads GetPrincipalId,
// GetCountry, GetSimpleAddressId and scans slots 1..12 with IsSlotOccupied and
// GetMiiEx. Blank STUBs left r3 as the incoming argument and the store data
// unwritten, so FFL rejected the Mii. Semantics mirror Cemu nn_act/iosu_act
// for its default offline account ("using account default in first slot").

namespace {
std::atomic<uint32_t> g_initialize_count{0};
constexpr std::u16string_view kMiiName = u"default";
constexpr char kCountry[] = "NN"; // Cemu: country index 0 has no table entry

void require_guest(const CPUState* cpu, uint32_t address, uint64_t length, const char* what) {
    if (!address || uint64_t(address) + length > cpu->guest_memory_size ||
        !rbrew_memory_accessible(cpu->mem, address, length))
        throw std::runtime_error(std::string("nn::act ") + what + " is outside accessible guest memory");
}

// Account slots are 1..12; 0xFE names the current account. Other values are
// outside the proven surface and fail explicitly instead of inventing a status.
bool slot_occupied(uint32_t raw) {
    const uint8_t slot = uint8_t(raw);
    if (slot == RBREW_ACT_SLOT_CURRENT || slot == 1) return true;
    if (slot <= RBREW_ACT_SLOT_COUNT) return false;
    throw std::runtime_error("nn::act slot number is outside the proven range");
}

void write_mii(CPUState* cpu, uint32_t address) {
    require_guest(cpu, address, 0x60, "FFLStoreData output");
    const auto mii = rbrew_act_default_mii(kMiiName);
    for (uint32_t i = 0; i < mii.size(); ++i) rbrew_write8(cpu->mem, address + i, mii[i]);
}

void Initialize(CPUState* cpu) { ++g_initialize_count; cpu->r[3] = 0; }
void Finalize(CPUState* cpu) {
    uint32_t count = g_initialize_count.load();
    while (count && !g_initialize_count.compare_exchange_weak(count, count - 1)) {}
    cpu->r[3] = 0;
}
void GetSlotNo(CPUState* cpu) { cpu->r[3] = 1; }
void IsSlotOccupied(CPUState* cpu) { cpu->r[3] = slot_occupied(cpu->r[3]) ? 1 : 0; }
void IsNetworkAccount(CPUState* cpu) { cpu->r[3] = 0; }
// Cemu returns 1; with 0 Splatoon attempts nn_fp.RegisterAccount.
void HasNfsAccount(CPUState* cpu) { cpu->r[3] = 1; }
void GetPrincipalId(CPUState* cpu) { cpu->r[3] = 0; }
void GetSimpleAddressId(CPUState* cpu) { cpu->r[3] = 0; }
void GetMii(CPUState* cpu) { write_mii(cpu, cpu->r[3]); cpu->r[3] = 0; }
void GetMiiEx(CPUState* cpu) {
    if (!slot_occupied(cpu->r[4])) {
        cpu->r[3] = RBREW_ACT_RESULT_ACCOUNT_DOES_NOT_EXIST; // output untouched
        return;
    }
    write_mii(cpu, cpu->r[3]);
    cpu->r[3] = 0;
}
// Gambit SCI FUN_02C6D4A0 asks for the parental-control slot of the account
// whose p_acct%d.* parental.xml keys it then reads (FUN_027D1D60); slot 0
// makes it report "SCI Error". decaf-emu (accountdata parentalControlSlotNo)
// and Cemu both give the default account parental slot 1.
void GetParentalControlSlotNoEx(CPUState* cpu) {
    if (!slot_occupied(cpu->r[4])) {
        cpu->r[3] = RBREW_ACT_RESULT_ACCOUNT_DOES_NOT_EXIST; // output untouched
        return;
    }
    require_guest(cpu, cpu->r[3], 1, "parental slot output");
    rbrew_write8(cpu->mem, cpu->r[3], 1);
    cpu->r[3] = 0;
}
void GetMiiName(CPUState* cpu) {
    const uint32_t out = cpu->r[3];
    require_guest(cpu, out, (kMiiName.size() + 1) * 2, "Mii name output");
    for (size_t i = 0; i < kMiiName.size(); ++i) rbrew_write16(cpu->mem, out + uint32_t(i * 2), kMiiName[i]);
    rbrew_write16(cpu->mem, out + uint32_t(kMiiName.size() * 2), 0);
    cpu->r[3] = 0;
}
void GetCountry(CPUState* cpu) {
    const uint32_t out = cpu->r[3];
    require_guest(cpu, out, sizeof(kCountry), "country output");
    for (uint32_t i = 0; i < sizeof(kCountry); ++i) rbrew_write8(cpu->mem, out + i, uint8_t(kCountry[i]));
    cpu->r[3] = 0;
}
// Offline account: empty NNID string.
void GetAccountId(CPUState* cpu) {
    require_guest(cpu, cpu->r[3], 1, "account id output");
    rbrew_write8(cpu->mem, cpu->r[3], 0);
    cpu->r[3] = 0;
}
}

uint16_t rbrew_ffl_crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0;
    for (size_t c = 0; c < length; ++c) {
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
        crc ^= data[c];
    }
    return crc;
}

RbrewFflStoreData rbrew_act_default_mii(std::u16string_view name) {
    if (name.empty() || name.size() > RBREW_ACT_MII_NAME_MAX)
        throw std::invalid_argument("Mii name must be 1..10 UTF-16 units");
    // Words are authored big-endian and then byte-swapped: the record stores
    // these halfwords little-endian. Every halfword is 1 except +0x02
    // (1 | 1<<12) and +0x3A (1 | 3<<9); +0x08 holds big-endian 0x00033334.
    RbrewFflStoreData mii{};
    auto put_le16 = [&](size_t at, uint16_t value) {
        mii[at] = uint8_t(value);
        mii[at + 1] = uint8_t(value >> 8);
    };
    for (size_t at = 0; at < mii.size(); at += 2) put_le16(at, 1);
    put_le16(0x02, 1 | (1 << 12));
    put_le16(0x3A, 1 | (3 << 9));
    // 0x00033334 as big-endian bytes 00 03 33 34, then halfword-swapped.
    mii[0x08] = 0x03; mii[0x09] = 0x00; mii[0x0A] = 0x34; mii[0x0B] = 0x33;
    for (size_t i = 0; i < RBREW_ACT_MII_NAME_MAX; ++i)
        put_le16(0x1A + i * 2, i < name.size() ? uint16_t(name[i]) : 0);
    // Search the big-endian counter at +0x04 until the CRC over the full
    // record (including its trailing CRC halfword) is zero.
    for (uint32_t counter = 0;; ++counter) {
        mii[4] = uint8_t(counter >> 24); mii[5] = uint8_t(counter >> 16);
        mii[6] = uint8_t(counter >> 8);  mii[7] = uint8_t(counter);
        if (rbrew_ffl_crc16(mii.data(), mii.size()) == 0) return mii;
        if (counter == UINT32_MAX) throw std::runtime_error("No CRC-valid default Mii exists");
    }
}

void rbrew_act_reset_for_tests() { g_initialize_count = 0; }

#define ACT_REG(fn, mangled) rbrew_register_func(cpu, ADDR_##mangled, fn)
void nn_act_register(CPUState* cpu) {
    ACT_REG(Initialize, Initialize__Q2_2nn3actFv);
    ACT_REG(Finalize, Finalize__Q2_2nn3actFv);
    ACT_REG(GetSlotNo, GetSlotNo__Q2_2nn3actFv);
    ACT_REG(IsSlotOccupied, IsSlotOccupied__Q2_2nn3actFUc);
    ACT_REG(IsNetworkAccount, IsNetworkAccount__Q2_2nn3actFv);
    ACT_REG(HasNfsAccount, HasNfsAccount__Q2_2nn3actFv);
    ACT_REG(GetPrincipalId, GetPrincipalId__Q2_2nn3actFv);
    ACT_REG(GetSimpleAddressId, GetSimpleAddressId__Q2_2nn3actFv);
    ACT_REG(GetMii, GetMii__Q2_2nn3actFP12FFLStoreData);
    ACT_REG(GetMiiEx, GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc);
    ACT_REG(GetMiiName, GetMiiName__Q2_2nn3actFPw);
    ACT_REG(GetCountry, GetCountry__Q2_2nn3actFPc);
    ACT_REG(GetAccountId, GetAccountId__Q2_2nn3actFPc);
    ACT_REG(GetParentalControlSlotNoEx, GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc);
}
#undef ACT_REG
