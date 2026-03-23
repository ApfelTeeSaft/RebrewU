// nsysnet / nn_ac / nlibcurl
//
// Maps Wii U nsysnet functions to POSIX/BSD sockets so game network code
// runs against real TCP/IP.
//
// Wii U sockaddr_in layout (BSD, big-endian in guest memory):
//   +0: uint8  sin_len    (= 0x10)
//   +1: uint8  sin_family (AF_INET = 2)
//   +2: uint16 sin_port   (network byte order)
//   +4: uint32 sin_addr   (network byte order)
//   +8: uint8  sin_zero[8]
//
// gethostbyname / getaddrinfo results are written into a static scratch
// area at NET_SCRATCH_BASE (0x00100000) which lies below all game code
// (RPX text starts at 0x02000000).
//
// nn_ac (network availability) stubs:
//   IsApplicationConnected → *result = true; return 0
//   Initialize/Connect/Close/GetStatus → return 0 (success)
//
// nlibcurl: curl_global_init_mem → 0 (CURLE_OK); cleanup → no-op

#include "../os_common.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32")
typedef int              socklen_t;
typedef unsigned short   sa_family_t;
typedef int              ssize_t;
#  define CLOSE_SOCKET(fd) closesocket(fd)
#  define SOCK_ERRNO       WSAGetLastError()
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <poll.h>
#  define CLOSE_SOCKET(fd) close(fd)
#  define SOCK_ERRNO       errno
#  define INVALID_SOCKET   (-1)
#endif

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Scratch area for gethostbyname / getaddrinfo / inet_ntoa results.
// All offsets are relative to NET_SCRATCH_BASE in the guest arena.
// ---------------------------------------------------------------------------
static constexpr uint32_t NET_SCRATCH_BASE = 0x00100000u; // safely below game code

// hostent scratch (128 bytes)
static constexpr uint32_t HE_BASE          = NET_SCRATCH_BASE + 0x000u;
static constexpr uint32_t HE_ALIASES_ARR   = NET_SCRATCH_BASE + 0x020u; // uint32[2] = {0,0}
static constexpr uint32_t HE_ADDRLIST_ARR  = NET_SCRATCH_BASE + 0x028u; // uint32[2] = {addr,0}
static constexpr uint32_t HE_ADDR          = NET_SCRATCH_BASE + 0x030u; // 4-byte IP
static constexpr uint32_t HE_NAME_STR      = NET_SCRATCH_BASE + 0x040u; // hostname (64 chars)

// addrinfo scratch (up to 4 entries, each 32-byte struct + 16-byte sockaddr_in)
static constexpr uint32_t AI_BASE          = NET_SCRATCH_BASE + 0x100u;
static constexpr uint32_t AI_ENTRY_STRIDE  = 48u; // 32 (addrinfo) + 16 (sockaddr_in)
static constexpr uint32_t AI_MAX_ENTRIES   = 4u;
static constexpr uint32_t AI_CANON_STR     = NET_SCRATCH_BASE + 0x200u; // 64 chars

// inet_ntoa result buffer (cycling through 4 slots to match POSIX static behaviour)
static constexpr uint32_t INET_NTOA_BASE   = NET_SCRATCH_BASE + 0x280u;
static constexpr uint32_t INET_NTOA_SLOT   = 20u; // max "255.255.255.255\0" = 16 chars
static uint32_t           s_ntoa_slot      = 0;

// ---------------------------------------------------------------------------
// sockaddr_in guest ↔ host conversion
// ---------------------------------------------------------------------------
static void guest_to_host_sockaddr(const uint8_t* mem, uint32_t gptr,
                                   struct sockaddr_in* out)
{
    memset(out, 0, sizeof(*out));
    if (!gptr) return;
    out->sin_family = (sa_family_t)mem[gptr + 1]; // byte 1 = sin_family
    memcpy(&out->sin_port,         mem + gptr + 2, 2); // already network order
    memcpy(&out->sin_addr.s_addr,  mem + gptr + 4, 4); // already network order
}

static void host_to_guest_sockaddr(uint8_t* mem, uint32_t gptr,
                                   const struct sockaddr_in* in)
{
    if (!gptr) return;
    mem[gptr + 0] = 0x10;                          // sin_len = 16
    mem[gptr + 1] = (uint8_t)in->sin_family;       // sin_family
    memcpy(mem + gptr + 2, &in->sin_port,          2); // network order
    memcpy(mem + gptr + 4, &in->sin_addr.s_addr,   4); // network order
    memset(mem + gptr + 8, 0, 8);                  // sin_zero
}

// ---------------------------------------------------------------------------
// setsockopt level/option mapping: Wii U BSD → Linux POSIX
// ---------------------------------------------------------------------------
static int map_level(int wii_level) {
    if (wii_level == 0xFFFF) return SOL_SOCKET;
    if (wii_level == 6)      return IPPROTO_TCP;
    if (wii_level == 0)      return IPPROTO_IP;
    return wii_level;
}

static int map_so_option(int wii_opt) {
    // Map BSD SOL_SOCKET option values (Wii U) to Linux equivalents
    switch (wii_opt) {
        case 0x0001: return SO_DEBUG;
        case 0x0004: return SO_REUSEADDR;
        case 0x0008: return SO_KEEPALIVE;
        case 0x0010: return SO_DONTROUTE;
        case 0x0020: return SO_BROADCAST;
        case 0x0080: return SO_LINGER;
        case 0x0100: return SO_OOBINLINE;
        case 0x1001: return SO_SNDBUF;
        case 0x1002: return SO_RCVBUF;
        case 0x1007: return SO_ERROR;
        case 0x1008: return SO_TYPE;
        default:     return wii_opt;
    }
}

// ---------------------------------------------------------------------------
// select() fd_set conversion
// Wii U fd_set: big-endian uint32 bitmap (bit N = fd N, from LSB)
// Linux fd_set: same bit-numbering but stored little-endian internally.
// Since rbrew_read32 byte-swaps, we recover the logical bit values correctly.
// ---------------------------------------------------------------------------
static fd_set guest_to_fdset(uint8_t* mem, uint32_t gptr) {
    fd_set out;
    FD_ZERO(&out);
    if (!gptr) return out;
    uint32_t bits = rbrew_read32(mem, gptr);
    for (int i = 0; i < 32; i++) {
        if (bits & (1u << i)) FD_SET(i, &out);
    }
    return out;
}

static void fdset_to_guest(uint8_t* mem, uint32_t gptr, const fd_set* s) {
    if (!gptr) return;
    uint32_t bits = 0;
    for (int i = 0; i < 32; i++) {
        if (FD_ISSET(i, s)) bits |= (1u << i);
    }
    rbrew_write32(mem, gptr, bits);
}

// ---------------------------------------------------------------------------
// socket_lib_init / NSSLInit — initialise the socket library
// On Windows, calls WSAStartup; on POSIX this is a no-op.
// ---------------------------------------------------------------------------
static void socket_lib_init(CPUState* cpu) {
#if defined(_WIN32)
    WSADATA wd;
    WSAStartup(MAKEWORD(2,2), &wd);
#endif
    fprintf(stderr, "[nsysnet] socket_lib_init\n");
    RET = 0;
}

static void NSSLInit(CPUState* cpu)             { (void)cpu; RET = 0; }
static void set_resolver_allocator(CPUState* cpu){ (void)cpu; }

// ---------------------------------------------------------------------------
// mw_socket / socketclose
// On Windows, SOCKET is UINT_PTR but games always get small FD numbers so
// casting to int is safe (Wii U socket descriptors fit in int).
// ---------------------------------------------------------------------------
static void mw_socket(CPUState* cpu) {
    // ARG0=domain, ARG1=type, ARG2=protocol
    // Wii U domain/type constants match POSIX (AF_INET=2, SOCK_STREAM=1, SOCK_DGRAM=2)
#if defined(_WIN32)
    SOCKET s = socket((int)ARG0, (int)ARG1, (int)ARG2);
    RET = (s == INVALID_SOCKET) ? (uint32_t)-1 : (uint32_t)(int)s;
#else
    int fd = socket((int)ARG0, (int)ARG1, (int)ARG2);
    RET = (uint32_t)fd;
#endif
}

static void socketclose(CPUState* cpu) {
    RET = (uint32_t)CLOSE_SOCKET((int)ARG0);
}

static void socketlasterr(CPUState* cpu) {
    (void)cpu;
    RET = (uint32_t)SOCK_ERRNO;
}

// ---------------------------------------------------------------------------
// bind / connect / shutdown
// ---------------------------------------------------------------------------
static void bind_(CPUState* cpu) {
    struct sockaddr_in sa;
    guest_to_host_sockaddr(MEM, ARG1, &sa);
    RET = (uint32_t)bind((int)ARG0, (struct sockaddr*)&sa, (socklen_t)sizeof(sa));
}

// connect_ kept for completeness even though not in this game's PLT
static void connect_(CPUState* cpu) {
    struct sockaddr_in sa;
    guest_to_host_sockaddr(MEM, ARG1, &sa);
    RET = (uint32_t)connect((int)ARG0, (struct sockaddr*)&sa, (socklen_t)sizeof(sa));
}

static void shutdown_(CPUState* cpu) {
    // ARG1: how (0=SHUT_RD, 1=SHUT_WR, 2=SHUT_RDWR — same on Wii U and POSIX)
    RET = (uint32_t)shutdown((int)ARG0, (int)ARG1);
}

static void getsockname_(CPUState* cpu) {
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    int rc = getsockname((int)ARG0, (struct sockaddr*)&sa, &slen);
    if (rc == 0) host_to_guest_sockaddr(MEM, ARG1, &sa);
    if (ARG2) rbrew_write32(MEM, ARG2, 16u);
    RET = (uint32_t)rc;
}

// ---------------------------------------------------------------------------
// send / recv / sendto / recvfrom / recvfrom_ex
// ---------------------------------------------------------------------------
static void send_(CPUState* cpu) {
    // ARG0=fd, ARG1=buf_guest_ptr, ARG2=len, ARG3=flags
    uint32_t len = ARG2;
    if (!ARG1 || (uint64_t)ARG1 + len > WIIU_MEM_SIZE) { RET = (uint32_t)-1; return; }
    ssize_t n = send((int)ARG0, (const char*)(MEM + ARG1), (int)len, (int)ARG3);
    RET = (uint32_t)n;
}

static void recv_(CPUState* cpu) {
    // ARG0=fd, ARG1=buf_guest_ptr, ARG2=len, ARG3=flags
    uint32_t len = ARG2;
    if (!ARG1 || (uint64_t)ARG1 + len > WIIU_MEM_SIZE) { RET = (uint32_t)-1; return; }
    ssize_t n = recv((int)ARG0, (char*)(MEM + ARG1), (int)len, (int)ARG3);
    RET = (uint32_t)n;
}

static void sendto_(CPUState* cpu) {
    // ARG0=fd, ARG1=buf, ARG2=len, ARG3=flags, ARG4=dest_addr, ARG5=addrlen
    uint32_t len = ARG2;
    if (!ARG1 || (uint64_t)ARG1 + len > WIIU_MEM_SIZE) { RET = (uint32_t)-1; return; }
    struct sockaddr_in sa;
    guest_to_host_sockaddr(MEM, ARG4, &sa);
    ssize_t n = sendto((int)ARG0, (const char*)(MEM + ARG1), (int)len, (int)ARG3,
                       (struct sockaddr*)&sa, (socklen_t)sizeof(sa));
    RET = (uint32_t)n;
}

static void recvfrom_(CPUState* cpu) {
    // ARG0=fd, ARG1=buf, ARG2=len, ARG3=flags, ARG4=src_addr_out, ARG5=addrlen_out
    uint32_t len = ARG2;
    if (!ARG1 || (uint64_t)ARG1 + len > WIIU_MEM_SIZE) { RET = (uint32_t)-1; return; }
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    ssize_t n = recvfrom((int)ARG0, (char*)(MEM + ARG1), (int)len, (int)ARG3,
                         (struct sockaddr*)&sa, &slen);
    if (n >= 0 && ARG4) host_to_guest_sockaddr(MEM, ARG4, &sa);
    if (ARG5)            rbrew_write32(MEM, ARG5, 16u);
    RET = (uint32_t)n;
}

// recvfrom_ex is Wii U extension for receiving with extended info — treat as recvfrom
static void recvfrom_ex(CPUState* cpu) { recvfrom_(cpu); }

// sendto_multi: not emulated; return 0
static void sendto_multi(CPUState* cpu) { (void)cpu; RET = 0; }

// ---------------------------------------------------------------------------
// select
// ARG0=nfds, ARG1=readfds*, ARG2=writefds*, ARG3=exceptfds*, ARG4=timeval*
// Wii U timeval: +0 int32 sec (BE), +4 int32 usec (BE)
// ---------------------------------------------------------------------------
static void select_(CPUState* cpu) {
    int nfds = (int)ARG0;

    fd_set rfds = guest_to_fdset(MEM, ARG1);
    fd_set wfds = guest_to_fdset(MEM, ARG2);
    fd_set efds = guest_to_fdset(MEM, ARG3);

    struct timeval tv, *tvp = nullptr;
    if (ARG4 && (uint64_t)ARG4 + 8u <= WIIU_MEM_SIZE) {
        tv.tv_sec  = (long)(int32_t)rbrew_read32(MEM, ARG4 + 0u);
        tv.tv_usec = (long)(int32_t)rbrew_read32(MEM, ARG4 + 4u);
        tvp = &tv;
    }

    int rc = select(nfds,
                    ARG1 ? &rfds : nullptr,
                    ARG2 ? &wfds : nullptr,
                    ARG3 ? &efds : nullptr,
                    tvp);

    if (rc >= 0) {
        fdset_to_guest(MEM, ARG1, &rfds);
        fdset_to_guest(MEM, ARG2, &wfds);
        fdset_to_guest(MEM, ARG3, &efds);
    }
    RET = (uint32_t)rc;
}

// ---------------------------------------------------------------------------
// setsockopt
// ARG0=fd, ARG1=level, ARG2=optname, ARG3=optval_ptr, ARG4=optlen
// ---------------------------------------------------------------------------
static void setsockopt_(CPUState* cpu) {
    int fd      = (int)ARG0;
    int wlevel  = (int)ARG1;
    int wopt    = (int)ARG2;
    uint32_t vp = ARG3;
    uint32_t vl = ARG4;

    int hlevel = map_level(wlevel);
    int hopt   = (hlevel == SOL_SOCKET) ? map_so_option(wopt) : wopt;

    if (!vp || vl == 0 || (uint64_t)vp + vl > WIIU_MEM_SIZE) { RET = 0; return; }

    // The optval is a 4-byte integer for most options (endian-swap from BE guest)
    int val_host = (int)rbrew_read32(MEM, vp);
    int rc = setsockopt(fd, hlevel, hopt, (const char*)&val_host, (socklen_t)sizeof(val_host));
    // Ignore setsockopt failures (option mapping may be imperfect)
    RET = (rc < 0) ? 0u : 0u; // always return 0
}

// ---------------------------------------------------------------------------
// Byte-order functions: on Wii U (big-endian) these are all identity ops.
// The guest calls them with values already in "host" (= network) byte order.
// ---------------------------------------------------------------------------
static void htonl_(CPUState* cpu)  { RET = ARG0; }
static void htons_(CPUState* cpu)  { RET = ARG0 & 0xFFFFu; }
static void ntohl_(CPUState* cpu)  { RET = ARG0; }
static void ntohs_(CPUState* cpu)  { RET = ARG0 & 0xFFFFu; }

// ---------------------------------------------------------------------------
// inet_ntoa(in_addr)
// ARG0 = uint32 IP address (network byte order, as stored in big-endian guest)
// Returns guest ptr to a "255.255.255.255\0" string in the scratch area.
// ---------------------------------------------------------------------------
static void inet_ntoa_(CPUState* cpu) {
    uint32_t slot = s_ntoa_slot & 3u;
    s_ntoa_slot++;
    uint32_t gptr = INET_NTOA_BASE + slot * INET_NTOA_SLOT;

    // ARG0 holds the uint32 in big-endian; bytes are already in network order
    uint32_t addr_be = ARG0;
    // Extract octets from big-endian representation
    uint8_t b0 = (uint8_t)(addr_be >> 24);
    uint8_t b1 = (uint8_t)(addr_be >> 16);
    uint8_t b2 = (uint8_t)(addr_be >>  8);
    uint8_t b3 = (uint8_t)(addr_be      );

    char buf[20];
    int len = snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b0, b1, b2, b3);
    if (len > 0 && gptr + (uint32_t)len + 1u <= WIIU_MEM_SIZE) {
        memcpy(MEM + gptr, buf, (size_t)len + 1u);
        RET = gptr;
    } else {
        RET = 0;
    }
}

// ---------------------------------------------------------------------------
// inet_pton(family, src_str_ptr, dst_ptr)
// ARG0=AF_INET, ARG1=src_str (guest ptr), ARG2=dst (guest ptr, 4 bytes for AF_INET)
// ---------------------------------------------------------------------------
static void inet_pton_(CPUState* cpu) {
    if (ARG0 != 2u) { RET = 0; return; } // only AF_INET supported
    if (!ARG1 || !ARG2) { RET = 0; return; }

    // Read the string from guest memory (up to 64 chars)
    char buf[64] = {};
    for (int i = 0; i < 63; i++) {
        buf[i] = (char)MEM[ARG1 + (uint32_t)i];
        if (!buf[i]) break;
    }

    struct in_addr addr;
    int rc = inet_pton(AF_INET, buf, &addr);
    if (rc == 1 && (uint64_t)ARG2 + 4u <= WIIU_MEM_SIZE) {
        memcpy(MEM + ARG2, &addr.s_addr, 4); // network-order bytes
    }
    RET = (uint32_t)rc; // 1=success, 0=invalid, -1=unsupported family
}

// ---------------------------------------------------------------------------
// gethostbyname — write result into NET_SCRATCH hostent area
// Returns guest ptr to the hostent struct, or 0 on failure.
// ---------------------------------------------------------------------------
static void gethostbyname_(CPUState* cpu) {
    if (!ARG0) { RET = 0; return; }

    // Read hostname from guest memory
    char hostname[256] = {};
    for (int i = 0; i < 255; i++) {
        hostname[i] = (char)MEM[ARG0 + (uint32_t)i];
        if (!hostname[i]) break;
    }

    struct hostent* he = gethostbyname(hostname);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0]) {
        fprintf(stderr, "[nsysnet] gethostbyname(\"%s\"): failed\n", hostname);
        RET = 0;
        return;
    }

    // Write h_name string
    size_t nlen = strlen(he->h_name);
    if (nlen > 63) nlen = 63;
    memcpy(MEM + HE_NAME_STR, he->h_name, nlen + 1);

    // Write IP address
    memcpy(MEM + HE_ADDR, he->h_addr_list[0], 4);

    // h_aliases array: [0x00000000] (no aliases)
    rbrew_write32(MEM, HE_ALIASES_ARR, 0u);

    // h_addr_list array: [HE_ADDR, NULL]
    rbrew_write32(MEM, HE_ADDRLIST_ARR + 0u, HE_ADDR);
    rbrew_write32(MEM, HE_ADDRLIST_ARR + 4u, 0u);

    // hostent struct
    rbrew_write32(MEM, HE_BASE + 0x00u, HE_NAME_STR);      // h_name
    rbrew_write32(MEM, HE_BASE + 0x04u, HE_ALIASES_ARR);   // h_aliases
    rbrew_write32(MEM, HE_BASE + 0x08u, 2u);               // h_addrtype = AF_INET
    rbrew_write32(MEM, HE_BASE + 0x0Cu, 4u);               // h_length
    rbrew_write32(MEM, HE_BASE + 0x10u, HE_ADDRLIST_ARR);  // h_addr_list

    fprintf(stderr, "[nsysnet] gethostbyname(\"%s\") → %u.%u.%u.%u\n",
            hostname,
            MEM[HE_ADDR], MEM[HE_ADDR+1], MEM[HE_ADDR+2], MEM[HE_ADDR+3]);

    RET = HE_BASE;
}

// ---------------------------------------------------------------------------
// getaddrinfo / freeaddrinfo
// ARG0=hostname_ptr, ARG1=servname_ptr, ARG2=hints_ptr, ARG3=res_ptr (addrinfo**)
//
// Wii U addrinfo struct (big-endian):
//   +0x00: int32  ai_flags
//   +0x04: int32  ai_family
//   +0x08: int32  ai_socktype
//   +0x0C: int32  ai_protocol
//   +0x10: uint32 ai_addrlen
//   +0x14: uint32 ai_canonname (guest ptr)
//   +0x18: uint32 ai_addr     (guest ptr to sockaddr_in)
//   +0x1C: uint32 ai_next     (guest ptr)
// ---------------------------------------------------------------------------
static void getaddrinfo_(CPUState* cpu) {
    if (!ARG0 && !ARG1) { RET = (uint32_t)-1; return; }

    // Read hostname
    char hostname[256] = {};
    if (ARG0) {
        for (int i = 0; i < 255; i++) {
            hostname[i] = (char)MEM[ARG0 + (uint32_t)i];
            if (!hostname[i]) break;
        }
    }

    // Read service name
    char servname[64] = {};
    if (ARG1) {
        for (int i = 0; i < 63; i++) {
            servname[i] = (char)MEM[ARG1 + (uint32_t)i];
            if (!servname[i]) break;
        }
    }

    // Read hints (simplified: just pick up ai_socktype)
    int hint_socktype = SOCK_STREAM;
    if (ARG2) hint_socktype = (int)rbrew_read32(MEM, ARG2 + 0x08u);

    // Resolve via host
    struct addrinfo hints_h = {};
    hints_h.ai_family   = AF_INET;
    hints_h.ai_socktype = hint_socktype ? hint_socktype : SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(*hostname ? hostname : nullptr,
                         *servname ? servname : nullptr,
                         &hints_h, &res);
    if (rc != 0 || !res) {
        fprintf(stderr, "[nsysnet] getaddrinfo(\"%s\",\"%s\"): %s\n",
                hostname, servname, gai_strerror(rc));
        RET = (uint32_t)(rc ? rc : -1);
        return;
    }

    // Write up to AI_MAX_ENTRIES result entries into guest scratch
    uint32_t count = 0;
    uint32_t prev_guest = 0;
    for (struct addrinfo* a = res; a && count < AI_MAX_ENTRIES; a = a->ai_next, count++) {
        if (a->ai_family != AF_INET) continue;

        uint32_t entry_base = AI_BASE + count * AI_ENTRY_STRIDE;
        uint32_t sa_base    = entry_base + 32u; // sockaddr_in follows the addrinfo

        // Write sockaddr_in
        struct sockaddr_in* sin = (struct sockaddr_in*)a->ai_addr;
        host_to_guest_sockaddr(MEM, sa_base, sin);

        // Write canonname string
        if (a->ai_canonname) {
            size_t cnlen = strlen(a->ai_canonname);
            if (cnlen > 63) cnlen = 63;
            memcpy(MEM + AI_CANON_STR, a->ai_canonname, cnlen + 1);
        } else {
            MEM[AI_CANON_STR] = 0;
        }

        // Write addrinfo struct
        rbrew_write32(MEM, entry_base + 0x00u, 0u);             // ai_flags
        rbrew_write32(MEM, entry_base + 0x04u, 2u);             // ai_family = AF_INET
        rbrew_write32(MEM, entry_base + 0x08u, (uint32_t)a->ai_socktype);
        rbrew_write32(MEM, entry_base + 0x0Cu, (uint32_t)a->ai_protocol);
        rbrew_write32(MEM, entry_base + 0x10u, 16u);            // ai_addrlen
        rbrew_write32(MEM, entry_base + 0x14u, AI_CANON_STR);   // ai_canonname
        rbrew_write32(MEM, entry_base + 0x18u, sa_base);        // ai_addr
        rbrew_write32(MEM, entry_base + 0x1Cu, 0u);             // ai_next (filled below)

        // Link previous entry to this one
        if (prev_guest)
            rbrew_write32(MEM, prev_guest + 0x1Cu, entry_base);
        prev_guest = entry_base;
    }
    freeaddrinfo(res);

    if (count == 0) { RET = (uint32_t)-1; return; }

    // Write *res = first entry
    if (ARG3) rbrew_write32(MEM, ARG3, AI_BASE);

    fprintf(stderr, "[nsysnet] getaddrinfo(\"%s\",\"%s\") → %u entr%s\n",
            hostname, servname, count, count == 1 ? "y" : "ies");
    RET = 0;
}

// freeaddrinfo — results are in a static scratch area, nothing to free
static void freeaddrinfo_(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// nn_ac (network availability) — always report connected
// ---------------------------------------------------------------------------
static void ac_Initialize(CPUState* cpu)               { (void)cpu; RET = 0; }
static void ac_Connect(CPUState* cpu)                  { (void)cpu; RET = 0; }
static void ac_Close(CPUState* cpu)                    { (void)cpu; RET = 0; }
static void ac_GetLastErrorCode(CPUState* cpu)         { if (ARG0) rbrew_write32(MEM, ARG0, 0u); RET = 0; }
static void ac_GetAssignedAddress(CPUState* cpu)       { if (ARG0) rbrew_write32(MEM, ARG0, 0u); RET = 0; }
static void ac_GetAssignedSubnet(CPUState* cpu)        { if (ARG0) rbrew_write32(MEM, ARG0, 0u); RET = 0; }

// IsApplicationConnected(bool* result)  → *result = TRUE; return 0
static void ac_IsApplicationConnected(CPUState* cpu) {
    if (ARG0 && (uint64_t)ARG0 + 1u <= WIIU_MEM_SIZE)
        MEM[ARG0] = 1u; // *result = true
    RET = 0;
}

// ---------------------------------------------------------------------------
// nlibcurl — game links curl for HTTPS leaderboards / telemetry.
// Route through host libcurl (already in CMakeLists); just let the game call
// it directly via its own thunks.  For the PLT stubs we only need no-ops for
// the two global lifecycle calls.
// ---------------------------------------------------------------------------
static void curl_global_init_mem_(CPUState* cpu)  { (void)cpu; RET = 0; } // CURLE_OK
static void curl_global_cleanup_(CPUState* cpu)   { (void)cpu; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "../stubs/stubs_addrs.h"

void nsysnet_register(CPUState* cpu) {
    // nsysnet core
    rbrew_register_func(cpu, ADDR_socket_lib_init,       socket_lib_init);
    rbrew_register_func(cpu, ADDR_NSSLInit,              NSSLInit);
    rbrew_register_func(cpu, ADDR_set_resolver_allocator,set_resolver_allocator);
    rbrew_register_func(cpu, ADDR_mw_socket,             mw_socket);
    rbrew_register_func(cpu, ADDR_socketclose,           socketclose);
    rbrew_register_func(cpu, ADDR_socketlasterr,         socketlasterr);
    rbrew_register_func(cpu, ADDR_bind,                  bind_);
    rbrew_register_func(cpu, ADDR_shutdown,              shutdown_);
    rbrew_register_func(cpu, ADDR_getsockname,           getsockname_);
    rbrew_register_func(cpu, ADDR_send,                  send_);
    rbrew_register_func(cpu, ADDR_recv,                  recv_);
    rbrew_register_func(cpu, ADDR_sendto,                sendto_);
    rbrew_register_func(cpu, ADDR_recvfrom,              recvfrom_);
    rbrew_register_func(cpu, ADDR_recvfrom_ex,           recvfrom_ex);
    rbrew_register_func(cpu, ADDR_sendto_multi,          sendto_multi);
    rbrew_register_func(cpu, ADDR_select,                select_);
    rbrew_register_func(cpu, ADDR_setsockopt,            setsockopt_);
    // Note: connect/accept/listen are not in this game's PLT
    rbrew_register_func(cpu, ADDR_htonl,                 htonl_);
    rbrew_register_func(cpu, ADDR_htons,                 htons_);
    rbrew_register_func(cpu, ADDR_ntohl,                 ntohl_);
    rbrew_register_func(cpu, ADDR_ntohs,                 ntohs_);
    rbrew_register_func(cpu, ADDR_inet_ntoa,             inet_ntoa_);
    rbrew_register_func(cpu, ADDR_inet_pton,             inet_pton_);
    rbrew_register_func(cpu, ADDR_gethostbyname,         gethostbyname_);
    rbrew_register_func(cpu, ADDR_getaddrinfo,           getaddrinfo_);
    rbrew_register_func(cpu, ADDR_freeaddrinfo,          freeaddrinfo_);

    // nn_ac (C++ mangled names → use addresses directly)
    rbrew_register_func(cpu, ADDR_Initialize__Q2_2nn2acFv,              ac_Initialize);
    rbrew_register_func(cpu, ADDR_Connect__Q2_2nn2acFv,                 ac_Connect);
    rbrew_register_func(cpu, ADDR_Close__Q2_2nn2acFv,                   ac_Close);
    rbrew_register_func(cpu, ADDR_GetLastErrorCode__Q2_2nn2acFPUi,      ac_GetLastErrorCode);
    rbrew_register_func(cpu, ADDR_GetAssignedAddress__Q2_2nn2acFPUl,    ac_GetAssignedAddress);
    rbrew_register_func(cpu, ADDR_GetAssignedSubnet__Q2_2nn2acFPUl,     ac_GetAssignedSubnet);
    rbrew_register_func(cpu, ADDR_IsApplicationConnected__Q2_2nn2acFPb, ac_IsApplicationConnected);

    // nlibcurl lifecycle
    rbrew_register_func(cpu, ADDR_curl_global_init_mem,  curl_global_init_mem_);
    rbrew_register_func(cpu, ADDR_curl_global_cleanup,   curl_global_cleanup_);
}
