// LAN multiplayer: native port of the rmc-local-net BrainSlug module.
//
// Original module: "MKW LAN multiplayer" by Chadderz (Alex Chadwick) and MrBean35000vr, BSD
// licensed; this follows the v0.9-Luporion.2 fork by Luporion (several instances per host,
// session PIDs derived from the control port, automatic game port fallback):
//   https://github.com/Luporion/MKW-LAN-Dolphin-Fork/tree/v0.9-Luporion.2
//
// The module replaces Nintendo WFC with direct console-to-console play on the local network:
// every console broadcasts a small status packet to UDP ports 27900..27929, the WFC friend
// roster is filled with whoever answered, and the DWC matchmaking "status records" that would
// normally travel through the GameSpy servers are sent straight to the peer's control port.
//
// Split of the port:
//   - projects/mkwii/lan_patches.txt holds the module's instruction patches. The translator
//     bakes them into the game (--guest-patches), since translated code cannot be poked at run
//     time. Its four `bl`/thread-entry trampolines land on the natives at 0x819F00x0 below.
//   - This file holds the module's function replacements and its discovery thread. Everything
//     talks to the game through its own SO/DWC/OS functions and guest memory, exactly like the
//     module does on a Wii or in Dolphin, so the wire format stays compatible with consoles and
//     Dolphin instances running the same (Luporion.2) module.
//
// All of it is inert unless the translation was built with the "lan" patch set for PAL: every
// function-start registration below is a wrapper that otherwise calls straight through.

#include "abi_bridge.h"
#include "guest_abi/guest_region.h"
#include "guest_function_imports.h"
#include "guest_patch_set.h"
#include "guest_small_data.h"
#include "memory.h"
#include "runtime_log.h"
#include "timebase_contract.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace MkwLan {
namespace {

// ---------------------------------------------------------------------------------------------
// Addresses (PAL). Function addresses are the BrainSlug symbols resolved against RMCP01; they
// match projects/mkwii/MAP.txt.
// ---------------------------------------------------------------------------------------------

constexpr uint32_t kSOSocket = 0x801ECFF4;
constexpr uint32_t kSOBind = 0x801ED188;
constexpr uint32_t kSOClose = 0x801ED0E4;
constexpr uint32_t kSOGetSockName = 0x801ED358;
constexpr uint32_t kSORecvFrom = 0x801ED454;
constexpr uint32_t kSOSendTo = 0x801ED4A0;
constexpr uint32_t kSOFcntl = 0x801ED4EC;
constexpr uint32_t kSOGetHostID = 0x801EDE88;
constexpr uint32_t kOSGetCurrentThread = 0x801A98B0;
constexpr uint32_t kOSCreateThread = 0x801A9E84;
constexpr uint32_t kOSResumeThread = 0x801AA58C;
constexpr uint32_t kOSSleepTicks = 0x801AACA8;
constexpr uint32_t kOSGetTime = 0x801AAD5C;
constexpr uint32_t kVIWaitForRetrace = 0x801B99EC;
constexpr uint32_t kDwcInitGt2Socket = 0x800D2808;
constexpr uint32_t kDwcProcessStatusRecord = 0x800DC234;

// Port addresses of the module (port_addresses.c, PAL column).
constexpr uint32_t kFormUserRecord = 0x8066300C;     // mkw_form_user_record
constexpr uint32_t kFriendTrackingPtr = 0x809C2110;  // friend tracking structure
constexpr uint32_t kMenuDataPtr = 0x809C1E38;        // menu_data
constexpr uint32_t kRenderPopup = 0x806025B0;        // Render Popup

// r13-relative slot of the DWC gamedata pointer: the module reads it out of the
// `lwz r9, -0x68F4(r13)` at dwc_process_status_record + 0x14.
constexpr int32_t kDwcGamedataSdaOffset = -0x68F4;

// Native trampolines and module data in otherwise unused RAM beyond MEM1's 24 MiB
// (guest_abi/guest_versions.inc maps this block 1:1 for every region). The trampoline
// addresses are also written in projects/mkwii/lan_patches.txt.
constexpr uint32_t kTrampolinePopulateFriend = 0x819F0000;
constexpr uint32_t kTrampolineSleepAFrame = 0x819F0010;
constexpr uint32_t kTrampolineMarioKartChannel = 0x819F0020;
constexpr uint32_t kTrampolineNetworkThread = 0x819F0030;
constexpr uint32_t kPidStruct = 0x819F1000;           // size 0x28, referenced by the patch set
constexpr uint32_t kNetworkThread = 0x819F1100;       // OSThread, size 0x318
constexpr uint32_t kLanBmgStruct = 0x819F1800;        // struct bmg_struct, size 0x14
constexpr uint32_t kNetworkThreadStack = 0x819F2000;  // grows down from + kNetworkThreadStackSize
constexpr uint32_t kNetworkThreadStackSize = 0x4000;
constexpr uint32_t kLanBmg = 0x819F8000;

constexpr uint8_t kLanBmgData[] = {
#include "lan_mode_bmg.inc"
};
static_assert(kLanBmg + sizeof(kLanBmgData) <= 0x81A00000u, "lan.bmg must fit the module block");

// ---------------------------------------------------------------------------------------------
// Protocol constants (network_thread.c / network_thread.h).
// ---------------------------------------------------------------------------------------------

constexpr uint16_t kBroadcastPort = 27900;
constexpr int kBroadcastInterval = 60;
constexpr uint64_t kBroadcastTimeout = kBroadcastInterval * 5;
constexpr uint32_t kBroadcastMagic = ('R' << 24) | ('M' << 16) | ('C' << 8) | 'X';
constexpr int kMaxLanPeers = 30;
constexpr uint32_t kStatusMagic = 0xBB49CC4D;
constexpr uint32_t kStatusSizeMax = 512;
constexpr uint32_t kFakePublicIp = 0x08080808;
constexpr uint32_t kMiiSize = 0x4C;

// struct broadcast_packet (packed, big-endian on the wire).
constexpr uint32_t kBpMagic = 0x00, kBpPid = 0x04, kBpMii = 0x08, kBpCommunicationPort = 0x54,
                   kBpLongitude = 0x56, kBpLatitude = 0x58, kBpCountry = 0x5A, kBpState = 0x5B,
                   kBpStatus = 0x5C, kBpSize = 0x5D;

// struct dwc_gamedata / dwc_gamedata2 / gt2_socket (mkw.h, gamespy/gt2.h).
constexpr uint32_t kGdGamedata2 = 0x04, kGdSuspend = 0x28, kGdPlayerCount = 0x30, kGdPeers = 0x38,
                   kGdPublicPort = 0x6D0, kGdPublicIp = 0x6D4, kGdMatchState = 0x71C,
                   kGdMyPid = 0x778, kGdHostPid = 0x77C, kGdLastEnterType = 0x7D8,
                   kGdLastEnterPort = 0x7DA, kGdLastEnterIp = 0x7DC, kGdLastEnterPid = 0x860,
                   kGdLastEnterLength = 0x864, kGdLastEnterTimestamp = 0x868,
                   kGdSbSelfLookupComplete = 0x895, kGdPlayersAtConsole = 0x8A8;
constexpr uint32_t kPeerPlayerId = 0x00, kPeerPublicIp = 0x04, kPeerLocalIp = 0x08,
                   kPeerPublicPort = 0x0C, kPeerPrivatePort = 0x0E, kPeerCommunicationIp = 0x10,
                   kPeerCommunicationPort = 0x14, kPeerIndex = 0x16, kPeerUnk17 = 0x17,
                   kPeerPlayersAtConsole = 0x28;
constexpr uint32_t kGd2Socket = 0x00, kGd2OnlineStatus = 0x24, kGd2MyPid = 0x64;
constexpr uint32_t kGt2SocketFd = 0x00, kGt2SocketLocalPort = 0x08;
constexpr uint32_t kDwcMatchStateSvWaiting = 0x0C;

// struct menu_friend_list_friend (size 0x1D0) inside menu_friend_list_friend_full (0x1D8).
constexpr uint32_t kFfStride = 0x1D8, kFfFriend = 0x08;
constexpr uint32_t kFrFriendCode = 0x08, kFrFlags = 0x18, kFrMii = 0x22, kFrCountry = 0x70,
                   kFrState = 0x71, kFrLongitude = 0x74, kFrLatitude = 0x76, kFrMiiKnown = 0x1C8;

// Wii IOS sockets (rvl/so.h, fcntl.h).
constexpr uint32_t kAfInet = 2, kSockDgram = 2, kProtoUdp = 0;
constexpr uint32_t kFGetFl = 3, kFSetFl = 4, kONonblock = 4;

// ---------------------------------------------------------------------------------------------
// Activation.
// ---------------------------------------------------------------------------------------------

bool Active() {
    static const bool active = [] {
        const bool lanPatched = std::strcmp(GuestPatchSet::kName, "lan") == 0;
        if (lanPatched && GuestRegion::Current() != GuestRegion::Id::Pal) {
            RT_LOGF(RT_TAG_NET, "[lan] the LAN patch set is PAL only; LAN multiplayer disabled\n");
            return false;
        }
        if (lanPatched) {
            RT_LOGF(RT_TAG_NET, "[lan] LAN multiplayer enabled (rmc-local-net v0.9-Luporion.2 port)\n");
        }
        return lanPatched;
    }();
    return active;
}

// ---------------------------------------------------------------------------------------------
// Guest call helpers.
// ---------------------------------------------------------------------------------------------

// Calls a guest function through normal dispatch (translated code or another native).
template <typename... Args>
uint32_t Call(CpuContext* ctx, uint32_t address, Args... args) {
    const uint32_t values[] = {static_cast<uint32_t>(args)..., 0u};
    for (size_t i = 0; i + 1 < sizeof(values) / sizeof(values[0]); ++i) {
        ctx->gpr[3 + i] = values[i];
    }
    InvokeIndirectCpu(address, ctx);
    return ctx->gpr[3];
}

uint64_t GuestTime(CpuContext* ctx) {
    InvokeIndirectCpu(kOSGetTime, ctx);
    return (static_cast<uint64_t>(ctx->gpr[3]) << 32) | ctx->gpr[4];
}

// Scratch space below the current guest stack pointer, for buffers the module kept on its own
// stack (socket addresses, packets, message parameters). Guest callees store their LR at
// sp + 4, so the usable area starts at sp + 8.
class GuestStackFrame {
public:
    GuestStackFrame(CpuContext* ctx, uint32_t bytes) : ctx_(ctx), savedSp_(ctx->gpr[1]) {
        const uint32_t size = (bytes + 8u + 15u) & ~15u;
        const uint32_t sp = (savedSp_ - size) & ~15u;
        Memory::Write32(sp, savedSp_); // back chain
        Memory::Write32(sp + 4, 0);
        ctx_->gpr[1] = sp;
        base_ = sp + 8;
        for (uint32_t offset = 0; offset < bytes; offset += 4) {
            Memory::Write32(base_ + offset, 0);
        }
    }
    ~GuestStackFrame() { ctx_->gpr[1] = savedSp_; }
    GuestStackFrame(const GuestStackFrame&) = delete;
    GuestStackFrame& operator=(const GuestStackFrame&) = delete;

    uint32_t At(uint32_t offset) const { return base_ + offset; }

private:
    CpuContext* ctx_;
    uint32_t savedSp_;
    uint32_t base_ = 0;
};

// Saved guest argument registers, so a wrapper can do its own guest calls before handing the
// untouched arguments to the original function.
struct SavedArgs {
    explicit SavedArgs(const CpuContext* ctx) {
        for (size_t i = 0; i < gpr.size(); ++i) gpr[i] = ctx->gpr[3 + i];
    }
    void Restore(CpuContext* ctx) const {
        for (size_t i = 0; i < gpr.size(); ++i) ctx->gpr[3 + i] = gpr[i];
    }
    std::array<uint32_t, 8> gpr{};
};

// struct so_addr_t { u8 sa_len; u8 sa_family; u16 sa_port; u32 sa_addr; }
void WriteSockAddr(uint32_t at, uint16_t port, uint32_t ip) {
    Memory::Write8(at + 0, 8);
    Memory::Write8(at + 1, static_cast<uint8_t>(kAfInet));
    Memory::Write16(at + 2, port);
    Memory::Write32(at + 4, ip);
}

uint32_t DwcGamedata() {
    return Memory::Read32(static_cast<uint32_t>(GuestSmallData::kSda1Base + kDwcGamedataSdaOffset));
}

uint32_t MenuData() {
    const uint32_t holder = Memory::Read32(kMenuDataPtr);
    return holder ? Memory::Read32(holder) : 0;
}

uint32_t MenuPanel(uint32_t menuData, uint32_t panel) {
    return menuData ? Memory::Read32(menuData + (panel + 2) * 4) : 0;
}

// ---------------------------------------------------------------------------------------------
// Module state (main.c / network_thread.c globals).
// ---------------------------------------------------------------------------------------------

struct PeerInfo {
    uint32_t ip = 0;
    uint16_t communicationPort = 0;
    int8_t status = 0;
};

struct TrackedPeer {
    uint64_t lastUpdate = 0;
    uint32_t realPid = 0;
    uint16_t controlPort = 0;
    PeerInfo info;
    uint16_t longitude = 0;
    uint16_t latitude = 0;
    uint8_t country = 0;
    uint8_t state = 0;
    std::array<uint8_t, kMiiSize> mii{};
};

std::array<TrackedPeer, kMaxLanPeers> g_trackedPeers;
uint32_t g_netState = 0; // struct mkw_net_state *, connection_state at +0x28
uint32_t g_myFakePid = 0;
uint32_t g_localIp = 0;
bool g_networkThreadRunning = false;
uint64_t g_timestamp = 0;
int32_t g_broadcastSocket = -1;
int g_broadcastTimer = 0;
uint16_t g_broadcastPort = 0;
int8_t g_gameState = 0; // 1 is idling
uint16_t g_communicationPort = 0;
bool g_initialisingGameSocket = false;
bool g_haveMii = false;
std::array<uint8_t, kMiiSize> g_myMii{};
uint16_t g_myLongitude = 0;
uint16_t g_myLatitude = 0;
uint8_t g_myCountry = 0;
uint8_t g_myState = 0;
uint64_t g_timeInState1 = 0;

void SetGameState(int8_t state) {
    if (g_gameState != state) {
        RT_LOGF(RT_TAG_NET, "[lan] game_state %d -> %d\n", g_gameState, state);
    }
    g_gameState = state;
}

// ---------------------------------------------------------------------------------------------
// PIDs (main.c).
// ---------------------------------------------------------------------------------------------

void SetupPid(CpuContext* ctx, uint32_t gamedata) {
    while (g_myFakePid <= kMaxLanPeers) {
        g_myFakePid = static_cast<uint32_t>(GuestTime(ctx));
    }
    if (gamedata) {
        Memory::Write32(gamedata + kGdMyPid, g_myFakePid);
        const uint32_t gamedata2 = Memory::Read32(gamedata + kGdGamedata2);
        if (gamedata2) Memory::Write32(gamedata2 + kGd2MyPid, g_myFakePid);
    }
    Memory::Write32(kPidStruct + 0x1C, g_myFakePid);
    Memory::Write32(kPidStruct + 0x24, 0x524D434A); // RMCJ
}

// Called once per LAN login after the control socket has been bound. Several instances on one
// host can start with identical timer values, so five PID bits carry the port offset (0..29);
// the remaining timer/IP bits keep different machines apart. Bit 30 keeps the PID above the
// friend aliases 1..30 and below INT32_MAX. A session identifier, not a friend code.
void SetupLanPid(CpuContext* ctx, uint16_t controlPort) {
    static uint32_t pidBase = 0;
    if (pidBase == 0) {
        const uint64_t now = GuestTime(ctx);
        uint32_t seed = static_cast<uint32_t>(now) ^ static_cast<uint32_t>(now >> 32) ^
                        Call(ctx, kSOGetHostID);
        seed ^= seed >> 16;
        seed *= 0x7FEB352Du;
        seed ^= seed >> 15;
        pidBase = 0x40000000u | (seed & 0x3FFFFFE0u);
    }
    // Keep the PID on reconnect when the port is unchanged, otherwise the other consoles would
    // reserve another friend slot for the same player.
    g_myFakePid = pidBase | ((controlPort - kBroadcastPort) & 0x1Fu);
    SetupPid(ctx, DwcGamedata());
}

// ---------------------------------------------------------------------------------------------
// Peer tracking (network_thread.c).
// ---------------------------------------------------------------------------------------------

int FindOrAllocPeer(uint32_t ip, uint16_t port, uint32_t pid) {
    // Only our own advertisement is ignored (not our IP), so instances sharing a host see
    // each other; peers are identified by PID for the same reason.
    if (pid == 0 || pid == g_myFakePid) return -1;
    for (int i = 0; i < kMaxLanPeers; ++i) {
        if (g_trackedPeers[i].realPid == pid) {
            g_trackedPeers[i].info.ip = ip;
            return i;
        }
    }
    for (int i = 0; i < kMaxLanPeers; ++i) {
        if (g_trackedPeers[i].info.ip == 0) {
            g_trackedPeers[i].info.ip = ip;
            g_trackedPeers[i].realPid = pid;
            RT_LOGF(RT_TAG_NET, "[lan] discovered peer pid=%u ip=%08x control=%u\n", pid, ip, port);
            return i;
        }
    }
    return -1;
}

int FindPeerByFakePid(uint32_t fakePid) {
    if (fakePid == 0) return -1;
    if (fakePid < kMaxLanPeers + 1) return static_cast<int>(fakePid) - 1;
    return -1;
}

int FindPeerByPid(uint32_t pid) {
    if (pid == 0) return -1;
    if (pid < kMaxLanPeers + 1) return static_cast<int>(pid) - 1;
    for (int i = 0; i < kMaxLanPeers; ++i) {
        if (g_trackedPeers[i].realPid == pid) return i;
    }
    return -1;
}

PeerInfo GetPeerInfo(unsigned index) {
    return index < kMaxLanPeers ? g_trackedPeers[index].info : PeerInfo{};
}

void CopyMiiToGuest(uint32_t at, const std::array<uint8_t, kMiiSize>& mii) {
    for (uint32_t i = 0; i < kMiiSize; ++i) Memory::Write8(at + i, mii[i]);
}

void SetFriend(int index, uint32_t f) {
    const TrackedPeer& peer = g_trackedPeers[index];
    Memory::Write64(f + kFrFriendCode, static_cast<uint64_t>(index + 1));
    const uint32_t miiKnown = peer.info.status != 0 ? 1u : 0u;
    Memory::Write32(f + kFrMiiKnown, miiKnown);
    Memory::Write16(f + kFrLongitude, peer.longitude);
    Memory::Write16(f + kFrLatitude, peer.latitude);
    Memory::Write8(f + kFrCountry, peer.country);
    Memory::Write8(f + kFrState, peer.state);
    if (miiKnown) CopyMiiToGuest(f + kFrMii, peer.mii);
    Memory::Write16(f + kFrFlags, static_cast<uint16_t>(0x1388u | miiKnown));
}

void ProcessBroadcast(uint32_t packet, uint32_t ip, uint16_t port) {
    if (Memory::Read32(packet + kBpMagic) != kBroadcastMagic) return;
    const uint32_t pid = Memory::Read32(packet + kBpPid);
    const uint16_t communicationPort = Memory::Read16(packet + kBpCommunicationPort);
    // IDs 1..30 are reserved for local friend-list aliases.
    if (pid <= kMaxLanPeers || port == 0 || communicationPort == 0) return;
    const int index = FindOrAllocPeer(ip, port, pid);
    if (index == -1) return;
    TrackedPeer& peer = g_trackedPeers[index];
    peer.controlPort = port;
    peer.lastUpdate = g_timestamp;
    peer.realPid = pid;
    peer.info.communicationPort = communicationPort;
    peer.info.status = static_cast<int8_t>(static_cast<int8_t>(Memory::Read8(packet + kBpStatus)) + 1);
    peer.longitude = Memory::Read16(packet + kBpLongitude);
    peer.latitude = Memory::Read16(packet + kBpLatitude);
    peer.country = Memory::Read8(packet + kBpCountry);
    peer.state = Memory::Read8(packet + kBpState);
    for (uint32_t i = 0; i < kMiiSize; ++i) peer.mii[i] = Memory::Read8(packet + kBpMii + i);

    // Fill in the Mii of a roster entry that is already on screen.
    const uint32_t menuData = MenuData();
    const uint32_t ff = menuData ? Memory::Read32(menuData + 0x404) : 0;
    const uint32_t f = ff ? ff + static_cast<uint32_t>(index) * kFfStride + kFfFriend : 0;
    if (f && Memory::Read32(f + kFrMiiKnown) == 0) {
        Memory::Write16(f + kFrLongitude, peer.longitude);
        Memory::Write16(f + kFrLatitude, peer.latitude);
        Memory::Write8(f + kFrCountry, peer.country);
        Memory::Write8(f + kFrState, peer.state);
        Memory::Write32(f + kFrMiiKnown, 1);
        CopyMiiToGuest(f + kFrMii, peer.mii);
        Memory::Write16(f + kFrFlags, static_cast<uint16_t>(Memory::Read16(f + kFrFlags) | 1u));
    }
}

// A status message carries its sender's PID and game port, and arrives from its control port.
// Remember the sender, so a reply has somewhere to go even when its broadcasts never reach us
// (a firewall, a VPN or virtual adapter, Wi-Fi isolation). Without this the host could not
// answer a join request from such a console, and DWC closed the whole room.
void NoteStatusSender(uint32_t pid, uint32_t ip, uint16_t port, uint16_t communicationPort) {
    if (pid <= kMaxLanPeers || ip == 0 || port == 0) return;
    const bool known = FindPeerByPid(pid) != -1;
    const int index = FindOrAllocPeer(ip, port, pid);
    if (index == -1) return;
    TrackedPeer& peer = g_trackedPeers[index];
    peer.controlPort = port;
    if (communicationPort != 0) peer.info.communicationPort = communicationPort;
    if (!known) {
        RT_LOGF(RT_TAG_NET, "[lan] learned peer pid=%u from its status message (no broadcast seen)\n",
                pid);
    }
}

void ProcessStatus(CpuContext* ctx, uint32_t buffer, int32_t length, uint32_t ip, uint16_t port) {
    RT_LOGF(RT_TAG_NET, "[lan] receive size %d from %08x:%u\n", length, ip, port);
    if (length < 0x14) return;
    const uint32_t header = Memory::Read32(buffer + 8);
    if (length < 0x14 + static_cast<int32_t>((header >> 16) & 0xFF)) return;
    if (Memory::Read32(buffer) != kStatusMagic) return;
    RT_LOGF(RT_TAG_NET, "[lan] receive status message %u from %08x:%u\n", header >> 24, ip, port);
    NoteStatusSender(__builtin_bswap32(Memory::Read32(buffer + 16)), ip, port,
                     static_cast<uint16_t>(header & 0xFFFF));
    const uint32_t gamedata = DwcGamedata();
    if (gamedata == 0) return;
    Memory::Write32(gamedata + kGdPublicIp, kFakePublicIp);
    Call(ctx, kDwcProcessStatusRecord, header >> 24, __builtin_bswap32(Memory::Read32(buffer + 16)),
         ip, port, buffer + 20, (header >> 18) & 0x3F);
}

void CheckForStatusOrBroadcast(CpuContext* ctx) {
    int32_t r = 0;
    do {
        GuestStackFrame frame(ctx, kStatusSizeMax + 8);
        const uint32_t buffer = frame.At(0);
        const uint32_t raddr = frame.At(kStatusSizeMax);
        Memory::Write8(raddr, 8); // sa_len
        r = static_cast<int32_t>(Call(ctx, kSORecvFrom, g_broadcastSocket, buffer, kStatusSizeMax, 0, raddr));
        if (r <= 0) break;
        if (r < 4) {
            RT_LOGF(RT_TAG_NET, "[lan] received overly short message, %d bytes\n", r);
            continue;
        }
        const uint32_t ip = Memory::Read32(raddr + 4);
        const uint16_t port = Memory::Read16(raddr + 2);
        const uint32_t magic = Memory::Read32(buffer);
        if (magic == kStatusMagic) {
            ProcessStatus(ctx, buffer, r, ip, port);
        } else if (magic == kBroadcastMagic) {
            if (r != static_cast<int32_t>(kBpSize)) continue;
            ProcessBroadcast(buffer, ip, port);
        } else {
            RT_LOGF(RT_TAG_NET, "[lan] received unknown message, %d bytes, header %08x\n", r, magic);
        }
    } while (r > 0);
    for (TrackedPeer& peer : g_trackedPeers) {
        if (peer.lastUpdate + kBroadcastTimeout < g_timestamp) peer.info.status = 0;
    }
}

// Disconnects a friend room join that got stuck (network_thread.c check_for_stuck_friend_join).
void CheckForStuckFriendJoin(CpuContext* ctx, uint32_t gamedata) {
    const uint32_t menuData = MenuData();
    const uint32_t panel9b = MenuPanel(menuData, 0x9B);
    const uint32_t state = panel9b ? panel9b + 0x35C : 0;
    const uint32_t startState = state ? Memory::Read32(state) : 0;
    const uint32_t gamedata2 = Memory::Read32(gamedata + kGdGamedata2);
    const bool backOnMenu = gamedata2 && Memory::Read32(gamedata2 + kGd2OnlineStatus) == 3;
    const bool stuckJoining =
        (g_gameState == 4 || g_gameState == 13) && state && startState == 1 &&
        (Memory::Read32(gamedata + kGdMatchState) == kDwcMatchStateSvWaiting ||
         (GuestTime(ctx) - g_timeInState1) / TimeBaseContract::kTicksPerSecond >= 65);
    if (!backOnMenu && !stuckJoining) return;

    SetGameState(0);
    // Leave the state alone while a dialogue (panel 4d) is open - probably a DC message.
    const uint32_t panel4d = MenuPanel(menuData, 0x4D);
    if (state && panel4d && Memory::Read32(panel4d + 8) == 1) {
        if (startState == 1 || startState == 3) Memory::Write32(state, 2);
        RT_LOGF(RT_TAG_NET, "[lan] panel9b.state %u -> %u\n", startState, Memory::Read32(state));
    }
}

void UpdateGameState(CpuContext* ctx, uint32_t gamedata) {
    if (g_gameState == 0) return;
    CheckForStuckFriendJoin(ctx, gamedata);
    const bool suspended = Memory::Read32(gamedata + kGdSuspend) != 0;
    switch (g_gameState) {
    case 3: if (suspended) SetGameState(11); break;
    case 4: if (suspended) SetGameState(13); break;
    case 11: if (!suspended) SetGameState(3); break;
    case 13: if (!suspended) SetGameState(4); break;
    default: break;
    }
}

bool ComputeAndBroadcastStatus(CpuContext* ctx) {
    const uint32_t gamedata = DwcGamedata();
    if (gamedata == 0) return false; // disconnected from "WFC"
    UpdateGameState(ctx, gamedata);

    GuestStackFrame frame(ctx, 0x60 + 8);
    const uint32_t packet = frame.At(0);
    const uint32_t baddr = frame.At(0x60);
    Memory::Write32(packet + kBpMagic, kBroadcastMagic);
    Memory::Write32(packet + kBpPid, g_myFakePid);
    Memory::Write16(packet + kBpCommunicationPort, g_communicationPort);
    Memory::Write8(packet + kBpStatus, static_cast<uint8_t>(g_gameState));
    Memory::Write8(packet + kBpCountry, 0xFF);
    Memory::Write8(packet + kBpState, 0xFF);
    if (g_haveMii) {
        CopyMiiToGuest(packet + kBpMii, g_myMii);
        Memory::Write16(packet + kBpLongitude, g_myLongitude);
        Memory::Write16(packet + kBpLatitude, g_myLatitude);
        Memory::Write8(packet + kBpCountry, g_myCountry);
        Memory::Write8(packet + kBpState, g_myState);
    }
    for (int i = 0; i < kMaxLanPeers; ++i) {
        WriteSockAddr(baddr, static_cast<uint16_t>(kBroadcastPort + i), 0xFFFFFFFFu);
        Call(ctx, kSOSendTo, g_broadcastSocket, packet, kBpSize, 0, baddr);
    }
    return true;
}

bool Initialise(CpuContext* ctx) {
    SetGameState(0);
    g_broadcastTimer = kBroadcastInterval;
    g_localIp = Call(ctx, kSOGetHostID);
    if (g_localIp == 0) {
        RT_LOGF(RT_TAG_NET, "[lan] SOGetHostID returned 0\n");
        return false;
    }

    g_broadcastSocket = static_cast<int32_t>(Call(ctx, kSOSocket, kAfInet, kSockDgram, kProtoUdp));
    if (g_broadcastSocket < 0) {
        RT_LOGF(RT_TAG_NET, "[lan] SOSocket returned %d\n", g_broadcastSocket);
        return false;
    }

    int32_t r = -1;
    {
        GuestStackFrame frame(ctx, 8);
        const uint32_t baddr = frame.At(0);
        for (int i = 0; i < kMaxLanPeers; ++i) {
            const uint16_t port = static_cast<uint16_t>(kBroadcastPort + i);
            WriteSockAddr(baddr, port, 0);
            r = static_cast<int32_t>(Call(ctx, kSOBind, g_broadcastSocket, baddr));
            if (r >= 0) {
                g_broadcastPort = port;
                RT_LOGF(RT_TAG_NET, "[lan] broadcast socket bound to port %u\n", port);
                break;
            }
        }
    }
    if (r < 0) {
        RT_LOGF(RT_TAG_NET, "[lan] could not bind any broadcast port\n");
        return false;
    }

    const int32_t flags = static_cast<int32_t>(Call(ctx, kSOFcntl, g_broadcastSocket, kFGetFl, 0));
    r = flags < 0 ? flags
                  : static_cast<int32_t>(Call(ctx, kSOFcntl, g_broadcastSocket, kFSetFl,
                                              static_cast<uint32_t>(flags) | kONonblock));
    if (r < 0) {
        RT_LOGF(RT_TAG_NET, "[lan] set_nonblock returned %d\n", r);
        return false;
    }

    // The bound port tells simultaneous instances on one host apart; fix the PID before the
    // game socket is created or advertised.
    SetupLanPid(ctx, g_broadcastPort);
    RT_LOGF(RT_TAG_NET, "[lan] local peer pid=%u ip=%08x control=%u\n", g_myFakePid, g_localIp,
            g_broadcastPort);

    g_initialisingGameSocket = true;
    r = static_cast<int32_t>(Call(ctx, kDwcInitGt2Socket));
    g_initialisingGameSocket = false;
    if (r != 0) {
        RT_LOGF(RT_TAG_NET, "[lan] dwc_init_gt2_socket returned %d\n", r);
        return false;
    }

    // Advertise the port the game socket really owns, including a fallback port.
    const uint32_t gamedata = DwcGamedata();
    const uint32_t gamedata2 = gamedata ? Memory::Read32(gamedata + kGdGamedata2) : 0;
    const uint32_t gt2Socket = gamedata2 ? Memory::Read32(gamedata2 + kGd2Socket) : 0;
    if (gt2Socket == 0) {
        RT_LOGF(RT_TAG_NET, "[lan] no GT2 socket after dwc_init_gt2_socket\n");
        return false;
    }
    uint16_t gamePort = 0;
    {
        GuestStackFrame frame(ctx, 8);
        const uint32_t gameAddr = frame.At(0);
        Memory::Write8(gameAddr, 8);
        r = static_cast<int32_t>(Call(ctx, kSOGetSockName, Memory::Read32(gt2Socket + kGt2SocketFd), gameAddr));
        gamePort = Memory::Read16(gameAddr + 2);
    }
    if (r < 0 || gamePort == 0) {
        RT_LOGF(RT_TAG_NET, "[lan] could not read bound game port (%d)\n", r);
        return false;
    }
    g_communicationPort = gamePort;
    Memory::Write16(gt2Socket + kGt2SocketLocalPort, gamePort);
    RT_LOGF(RT_TAG_NET, "[lan] game socket ready on port %u\n", gamePort);
    Memory::Write32(gamedata2 + kGd2OnlineStatus, 3);
    return true;
}

void FillInPeer0(uint32_t gamedata) {
    const uint32_t peer0 = gamedata + kGdPeers;
    Memory::Write16(gamedata + kGdPublicPort, g_communicationPort);
    Memory::Write32(gamedata + kGdPublicIp, kFakePublicIp);
    Memory::Write32(peer0 + kPeerPlayerId, Memory::Read32(gamedata + kGdMyPid));
    Memory::Write32(peer0 + kPeerPublicIp, kFakePublicIp);
    Memory::Write32(peer0 + kPeerLocalIp, g_localIp);
    Memory::Write16(peer0 + kPeerPublicPort, g_communicationPort);
    Memory::Write16(peer0 + kPeerPrivatePort, g_communicationPort);
    Memory::Write32(peer0 + kPeerCommunicationIp, 0);
    Memory::Write16(peer0 + kPeerCommunicationPort, 0);
    Memory::Write8(peer0 + kPeerUnk17, 1);
    Memory::Write32(peer0 + kPeerPlayersAtConsole, Memory::Read32(gamedata + kGdPlayersAtConsole));
}

bool SendStatusRecord(CpuContext* ctx, uint32_t type, uint32_t pid, uint32_t ip, uint16_t port,
                      uint32_t packet, uint32_t packetLenInWords) {
    // The wire header has one byte for the payload length in bytes.
    if (packetLenInWords > 63 || (packetLenInWords != 0 && packet == 0) || g_broadcastSocket < 0) {
        return false;
    }
    if (type == 2 || type == 6) {
        const uint32_t gamedata = DwcGamedata();
        if (gamedata) {
            Memory::Write8(gamedata + kGdLastEnterType, static_cast<uint8_t>(type));
            Memory::Write16(gamedata + kGdLastEnterPort, port);
            Memory::Write32(gamedata + kGdLastEnterIp, ip);
            Memory::Write32(gamedata + kGdLastEnterPid, pid);
            Memory::Write32(gamedata + kGdLastEnterLength, packetLenInWords);
            Memory::Write64(gamedata + kGdLastEnterTimestamp, GuestTime(ctx));
        }
    }

    const int index = FindPeerByPid(pid);
    if (index == -1) {
        RT_LOGF(RT_TAG_NET, "[lan] send_status_record to unknown peer %u\n", pid);
        return false;
    }
    const TrackedPeer& peer = g_trackedPeers[index];
    if (peer.info.ip == 0) {
        RT_LOGF(RT_TAG_NET, "[lan] send_status_record to peer with no ip\n");
        return false;
    }
    if (peer.controlPort == 0) {
        RT_LOGF(RT_TAG_NET, "[lan] send_status_record to peer with no control port\n");
        return false;
    }
    RT_LOGF(RT_TAG_NET, "[lan] send_status_record %u to %08x:%u\n", type, peer.info.ip, peer.controlPort);

    const uint32_t packetBytes = (packetLenInWords + 5) * 4;
    GuestStackFrame frame(ctx, packetBytes + 8);
    const uint32_t full = frame.At(0);
    const uint32_t daddr = frame.At(packetBytes);
    for (uint32_t i = 0; i < packetLenInWords; ++i) {
        Memory::Write32(full + (i + 5) * 4, Memory::Read32(packet + i * 4));
    }
    Memory::Write32(full + 0, kStatusMagic);
    Memory::Write32(full + 4, __builtin_bswap32(90u));
    Memory::Write32(full + 8, (type << 24) | ((packetLenInWords * 4) << 16) | g_communicationPort);
    Memory::Write32(full + 12, kFakePublicIp);
    Memory::Write32(full + 16, __builtin_bswap32(g_myFakePid));
    WriteSockAddr(daddr, peer.controlPort, peer.info.ip);
    const int32_t r = static_cast<int32_t>(Call(ctx, kSOSendTo, g_broadcastSocket, full, packetBytes, 0, daddr));
    if (r < 0) RT_LOGF(RT_TAG_NET, "[lan] SOSendTo returned %d\n", r);
    return r >= 0;
}

// ---------------------------------------------------------------------------------------------
// LAN menu texts (ui.c).
// ---------------------------------------------------------------------------------------------

uint32_t LanBmgStruct() {
    static bool installed = false;
    if (!installed) {
        for (uint32_t i = 0; i < sizeof(kLanBmgData); ++i) Memory::Write8(kLanBmg + i, kLanBmgData[i]);
        const uint32_t inf1 = kLanBmg + 0x20 + 8;
        const uint32_t dat1 = inf1 + Memory::Read32(inf1 - 4);
        const uint32_t mid1 = dat1 + Memory::Read32(dat1 - 4);
        Memory::Write32(kLanBmgStruct + 0x00, kLanBmg);
        Memory::Write32(kLanBmgStruct + 0x04, inf1);
        Memory::Write32(kLanBmgStruct + 0x08, dat1);
        Memory::Write32(kLanBmgStruct + 0x0C, 0);
        Memory::Write32(kLanBmgStruct + 0x10, mid1);
        installed = true;
    }
    return kLanBmgStruct;
}

uint32_t MapMessageId(uint32_t id) {
    switch (id) {
    case 0x07DD: case 0x0FA2: case 0x13BA: return 11000;
    case 0x07E8: return 11001;
    case 0x07E9: return 11002;
    case 0x07F1: return 11004;
    case 0x07F2: return 11005;
    case 0x10DC: return 11006;
    case 0x1389: return 11007;
    case 0x10DD: return 11008;
    case 0x138E: case 0x1A36: return 11009;
    case 0x10DE: return 11010;
    case 0x13F7: return 11011;
    case 0x1393: return 11014;
    case 0x1453: return 11015;
    case 0x1450: return 11016;
    case 0x100C: return 11019;
    case 0x1006: return 11020;
    case 0x0FB5: return 11021;
    case 0x0FB6: return 11022;
    case 0x1451: return 11023;
    case 0x14B4: return 11025;
    case 0x14B5: return 11026;
    case 0x0818: return 11027;
    case 0x0841: return 11028;
    case 0x0FAA: return 11029;
    case 0x0FAB: return 11030;
    case 0x1CEA: return 11031;
    case 0x1CEE: return 11032;
    case 0x26AF: return 11033;
    default:
        if (id == 0x14B9 || id == 0x14BA || (id >= 0x14BF && id <= 0x14CE)) return 11024;
        return id;
    }
}

void WriteIpParams(uint32_t at, uint32_t ip) {
    Memory::Write32(at + 0, (ip >> 24) & 0xFF);
    Memory::Write32(at + 4, (ip >> 16) & 0xFF);
    Memory::Write32(at + 8, (ip >> 8) & 0xFF);
    Memory::Write32(at + 12, ip & 0xFF);
}

void ShowPopup(CpuContext* ctx, uint32_t fromPanel, uint32_t message, uint32_t params) {
    const uint32_t menuData = MenuData();
    const uint32_t panel4d = MenuPanel(menuData, 0x4D);
    if (panel4d == 0 || fromPanel == 0) return;
    const uint32_t setText = Memory::Read32(Memory::Read32(panel4d) + 0x1A * 4);
    Call(ctx, setText, panel4d, message, params);
    Call(ctx, kRenderPopup, fromPanel, 0x4D, 0);
}

// ---------------------------------------------------------------------------------------------
// Network thread entry (runs as a guest thread created in DWC_LoginAsync).
// ---------------------------------------------------------------------------------------------

void NetworkThreadMain(CpuContext* ctx) {
    g_networkThreadRunning = true;
    RT_LOGF(RT_TAG_NET, "[lan] network_thread startup\n");
    if (!Initialise(ctx)) {
        if (g_netState) Memory::Write32(g_netState + 0x28, 5);
    } else {
        RT_LOGF(RT_TAG_NET, "[lan] network_thread initialised\n");
        if (g_netState) Memory::Write32(g_netState + 0x28, 5);
        while (true) {
            ++g_timestamp;
            if (++g_broadcastTimer >= kBroadcastInterval) {
                g_broadcastTimer = 0;
                if (!ComputeAndBroadcastStatus(ctx)) break;
            }
            CheckForStatusOrBroadcast(ctx);
            Call(ctx, kVIWaitForRetrace);
        }
    }

    g_initialisingGameSocket = false;
    g_communicationPort = 0;
    // Release the control port on normal exit and on a partial startup failure. The GT2
    // socket belongs to DWC and is not closed here.
    if (g_broadcastSocket >= 0) {
        Call(ctx, kSOClose, g_broadcastSocket);
        g_broadcastSocket = -1;
    }
    g_broadcastPort = 0;
    RT_LOGF(RT_TAG_NET, "[lan] network_thread exit\n");
    g_haveMii = false;
    g_trackedPeers = {};
    g_networkThreadRunning = false;
    ctx->gpr[3] = 0;
}

} // namespace
} // namespace MkwLan

// =============================================================================================
// Native entry points. Function-start registrations are wrappers: without the LAN patch set
// they call the next translation (base game or mod overlay) untouched.
// =============================================================================================

using namespace MkwLan;

// int DWC_LoginAsync(r3, r4, void (*r5)(int, int, void *), void *r6): instead of logging in to
// WFC, start the LAN discovery thread.
extern "C" void Lan_DWC_LoginAsync_800d13f8(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(800D13F8)(ctx);
    const uint32_t netState = ctx->gpr[6];
    SetupPid(ctx, DwcGamedata());

    // get_mii: the player's Mii and location from the current license's user record.
    {
        GuestStackFrame frame(ctx, 0xC8);
        const uint32_t record = frame.At(0);
        Call(ctx, kFormUserRecord, record);
        for (uint32_t i = 0; i < kMiiSize; ++i) g_myMii[i] = Memory::Read8(record + 16 + i);
        g_myLongitude = Memory::Read16(record + 0xBC);
        g_myLatitude = Memory::Read16(record + 0xBE);
        g_myCountry = Memory::Read8(record + 0xB8);
        g_myState = Memory::Read8(record + 0xB9);
        g_haveMii = true;
    }

    // setup_friends: 30 friend slots with the aliases 1..30.
    const uint32_t friends = Memory::Read32(kFriendTrackingPtr);
    if (friends) {
        for (uint32_t i = 0; i < kMaxLanPeers; ++i) {
            Memory::Write32(friends + 0x9C + i * 12 + 0, 0x3800);
            Memory::Write32(friends + 0x9C + i * 12 + 4, i + 1);
        }
    }

    g_netState = netState;
    if (g_networkThreadRunning) {
        RT_LOGF(RT_TAG_NET, "[lan] network thread still running; not starting a second one\n");
    } else {
        // Detached (the module passes false): nothing joins this thread, and a detached exit
        // fully releases the OSThread before the next LAN login creates it again.
        Call(ctx, kOSCreateThread, kNetworkThread, kTrampolineNetworkThread, 0,
             kNetworkThreadStack + kNetworkThreadStackSize, kNetworkThreadStackSize, 0x18, 1);
        Call(ctx, kOSResumeThread, kNetworkThread);
    }
    ctx->gpr[3] = 0;
}
REGISTER_NATIVE_WRAPPER_AS(0x800D13F8, Lan_DWC_LoginAsync_800d13f8, "Lan_DWC_LoginAsync_800d13f8");

// uint8_t dwc_get_friend_status(const struct dwc_buddy *buddy, uint8_t *loc, int *loc_length)
extern "C" void Lan_dwc_get_friend_status_800ce190(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(800CE190)(ctx);
    const uint32_t buddy = ctx->gpr[3];
    const uint32_t loc = ctx->gpr[4];
    const uint32_t locLength = ctx->gpr[5];
    const uint32_t pid = Memory::Read32(buddy + 4);
    const int index = FindPeerByFakePid(pid);
    if (index != -1 && g_trackedPeers[index].info.status != 0) {
        const int8_t status = g_trackedPeers[index].info.status;
        // struct { u32 gid; u8 region; u8 state; u8 player_count; u8 race_number; }
        Memory::Write32(loc + 0, pid);
        Memory::Write8(loc + 4, 0xFF);
        Memory::Write8(loc + 5, static_cast<uint8_t>((status - 1) & 0x3F));
        Memory::Write8(loc + 6, 12);
        Memory::Write8(loc + 7, static_cast<uint8_t>(status >> 6));
        Memory::Write32(locLength, 8);
        ctx->gpr[3] = 1;
    } else {
        Memory::Write32(locLength, 0xFFFFFFFFu);
        ctx->gpr[3] = 0;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x800CE190, Lan_dwc_get_friend_status_800ce190, "Lan_dwc_get_friend_status_800ce190");

// bool DWC_SetupGameServer(int player_count, ...): host a room.
extern "C" void Lan_DWC_SetupGameServer_800d1984(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(800D1984)(ctx);
    const SavedArgs args(ctx);
    const uint32_t gamedata = DwcGamedata();
    SetupPid(ctx, gamedata);
    args.Restore(ctx);
    MKW_GUEST_NEXT(800D1984)(ctx);
    const uint32_t result = ctx->gpr[3];
    RT_LOGF(RT_TAG_NET, "[lan] after server setup\n");
    if (gamedata) {
        FillInPeer0(gamedata);
        Memory::Write8(gamedata + kGdPeers + kPeerIndex, 0);
        Memory::Write32(gamedata + kGdPlayerCount, 1);
        Memory::Write32(gamedata + kGdMatchState, kDwcMatchStateSvWaiting);
    }
    SetGameState(3);
    ctx->gpr[3] = result;
}
REGISTER_NATIVE_WRAPPER_AS(0x800D1984, Lan_DWC_SetupGameServer_800d1984, "Lan_DWC_SetupGameServer_800d1984");

// bool DWC_ConnectToGameServerAsync(int player_count, ...): join a room.
extern "C" void Lan_DWC_ConnectToGameServerAsync_800d1ab4(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(800D1AB4)(ctx);
    const SavedArgs args(ctx);
    const uint32_t gamedata = DwcGamedata();
    SetupPid(ctx, gamedata);
    RT_LOGF(RT_TAG_NET, "[lan] on server join\n");
    if (gamedata) {
        FillInPeer0(gamedata);
        Memory::Write8(gamedata + kGdPeers + kPeerIndex, 0xFF);
        Memory::Write32(gamedata + kGdPlayerCount, 1);
        Memory::Write8(gamedata + kGdSbSelfLookupComplete, 1);
    }
    SetGameState(4);
    g_timeInState1 = GuestTime(ctx);
    args.Restore(ctx);
    MKW_GUEST_NEXT(800D1AB4)(ctx);
    const uint32_t result = ctx->gpr[3];
    if (gamedata) {
        const int index = FindPeerByPid(Memory::Read32(gamedata + kGdHostPid));
        if (index != -1) {
            Memory::Write32(gamedata + kGdHostPid, g_trackedPeers[index].realPid);
            RT_LOGF(RT_TAG_NET, "[lan] host_pid set to %u\n", g_trackedPeers[index].realPid);
        } else {
            RT_LOGF(RT_TAG_NET, "[lan] could not set host_pid\n");
        }
    }
    ctx->gpr[3] = result;
}
REGISTER_NATIVE_WRAPPER_AS(0x800D1AB4, Lan_DWC_ConnectToGameServerAsync_800d1ab4,
                           "Lan_DWC_ConnectToGameServerAsync_800d1ab4");

// int dwc_qr2_init(uint32_t pid) / int DWCi_SBUpdateAsync(uint32_t pid): no master server.
extern "C" void Lan_dwc_qr2_init_800d4e58(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(800D4E58)(ctx);
    ctx->gpr[3] = 0;
}
REGISTER_NATIVE_WRAPPER_AS(0x800D4E58, Lan_dwc_qr2_init_800d4e58, "Lan_dwc_qr2_init_800d4e58");

extern "C" void Lan_DWCi_SBUpdateAsync_800db614(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(800DB614)(ctx);
    ctx->gpr[3] = 0;
}
REGISTER_NATIVE_WRAPPER_AS(0x800DB614, Lan_DWCi_SBUpdateAsync_800db614, "Lan_DWCi_SBUpdateAsync_800db614");

// int dwc_send_status_record(int type, u32 pid, u32 ip, u16 port, const u32 *packet, size_t words)
extern "C" void Lan_dwc_send_status_record_800dbd38(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(800DBD38)(ctx);
    const uint32_t pid = ctx->gpr[4];
    const bool sent = SendStatusRecord(ctx, ctx->gpr[3], pid, ctx->gpr[5],
                                       static_cast<uint16_t>(ctx->gpr[6]), ctx->gpr[7], ctx->gpr[8]);
    // DWC treats a failed send as fatal and closes the room. For a host that punishes everybody
    // for one console it cannot reach, so the host counts it as a lost datagram instead; DWC's
    // own timeouts then drop the half-joined console. A joining console keeps the error, which
    // ends its join attempt quickly.
    const bool hosting = g_gameState == 3 || g_gameState == 11;
    if (!sent && hosting) {
        RT_LOGF(RT_TAG_NET, "[lan] host: status record to %u not delivered, room kept open\n", pid);
    }
    ctx->gpr[3] = sent || hosting ? 0 : 1;
}
REGISTER_NATIVE_WRAPPER_AS(0x800DBD38, Lan_dwc_send_status_record_800dbd38, "Lan_dwc_send_status_record_800dbd38");

// uint32_t ServerBrowserGetMyPublicIPAddr(void *sb)
extern "C" void Lan_ServerBrowserGetMyPublicIPAddr_8011e510(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(8011E510)(ctx);
    ctx->gpr[3] = kFakePublicIp;
}
REGISTER_NATIVE_WRAPPER_AS(0x8011E510, Lan_ServerBrowserGetMyPublicIPAddr_8011e510,
                           "Lan_ServerBrowserGetMyPublicIPAddr_8011e510");

// so_ret_t SOBind(so_fd_t fd, const so_addr_t *addr). DWC picks a random game port, which on a
// PC may already be taken; while the network thread creates the game socket, retry a failed
// bind with port 0 so the host picks a free one. Discovery stays on 27900..27929.
extern "C" void Lan_SOBind_801ed188(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(801ED188)(ctx);
    const SavedArgs args(ctx);
    const uint32_t fd = ctx->gpr[3];
    const uint32_t addr = ctx->gpr[4];
    MKW_GUEST_NEXT(801ED188)(ctx);
    const int32_t result = static_cast<int32_t>(ctx->gpr[3]);
    if (result >= 0 || !g_initialisingGameSocket || static_cast<int32_t>(fd) == g_broadcastSocket ||
        addr == 0 || Memory::Read8(addr + 1) != kAfInet || Memory::Read16(addr + 2) == 0) {
        return;
    }
    if (Call(ctx, kOSGetCurrentThread) != kNetworkThread) {
        ctx->gpr[3] = static_cast<uint32_t>(result);
        return;
    }
    RT_LOGF(RT_TAG_NET, "[lan] game port %u bind failed (%d); requesting a free port\n",
            Memory::Read16(addr + 2), result);
    GuestStackFrame frame(ctx, 8);
    const uint32_t fallback = frame.At(0);
    WriteSockAddr(fallback, 0, Memory::Read32(addr + 4));
    args.Restore(ctx);
    ctx->gpr[4] = fallback;
    MKW_GUEST_NEXT(801ED188)(ctx);
    if (static_cast<int32_t>(ctx->gpr[3]) < 0) {
        RT_LOGF(RT_TAG_NET, "[lan] automatic game port bind failed (%d)\n", static_cast<int32_t>(ctx->gpr[3]));
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x801ED188, Lan_SOBind_801ed188, "Lan_SOBind_801ed188");

// void BMG_Print(void *r3, struct bmg_struct *bmg1, struct bmg_struct *bmg2, int messageId,
//                void *parameters): swap WFC texts for the LAN ones.
extern "C" void Lan_BMG_Print_805cdd00(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(805CDD00)(ctx);
    uint32_t messageId = MapMessageId(ctx->gpr[6]);
    GuestStackFrame frame(ctx, 16);
    if (ctx->gpr[6] == 0x083E) {
        // 11012 shows an IP, 11013 "not connected".
        const uint32_t params = ctx->gpr[7];
        const uint32_t p0 = params ? Memory::Read32(params) : 1;
        const uint32_t p1 = params ? Memory::Read32(params + 4) : 1;
        const int32_t p2 = params ? static_cast<int32_t>(Memory::Read32(params + 8)) : 0;
        if (params && p0 == 0 && p1 == 0 && p2 > 0 && p2 < 31) {
            const PeerInfo peer = GetPeerInfo(static_cast<unsigned>(p2 - 1));
            if (peer.ip == 0) {
                messageId = 11013;
            } else {
                WriteIpParams(frame.At(0), peer.ip);
                ctx->gpr[7] = frame.At(0);
                messageId = 11012;
            }
        } else if (g_networkThreadRunning) {
            WriteIpParams(frame.At(0), g_localIp);
            ctx->gpr[7] = frame.At(0);
            messageId = 11012;
        }
    }
    if (messageId >= 11000 && messageId < 11050) ctx->gpr[4] = LanBmgStruct();
    ctx->gpr[6] = messageId;
    MKW_GUEST_NEXT(805CDD00)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805CDD00, Lan_BMG_Print_805cdd00, "Lan_BMG_Print_805cdd00");

// Remove Friend pressed (myPanel, thisButton): show the peer's endpoint instead.
extern "C" void Lan_OnRemoveFriendPressed_805d690c(CpuContext* ctx) {
    if (!Active()) return MKW_GUEST_NEXT(805D690C)(ctx);
    const uint32_t myPanel = ctx->gpr[3];
    const PeerInfo peer = GetPeerInfo(Memory::Read32(myPanel + 0x1108));
    if (peer.ip == 0) {
        ShowPopup(ctx, myPanel, 11018, 0);
        return;
    }
    GuestStackFrame frame(ctx, 24);
    WriteIpParams(frame.At(0), peer.ip);
    Memory::Write32(frame.At(16), peer.communicationPort);
    Memory::Write32(frame.At(20), static_cast<uint32_t>(static_cast<int32_t>(peer.status)));
    ShowPopup(ctx, myPanel, 11017, frame.At(0));
}
REGISTER_NATIVE_WRAPPER_AS(0x805D690C, Lan_OnRemoveFriendPressed_805d690c, "Lan_OnRemoveFriendPressed_805d690c");

// ---------------------------------------------------------------------------------------------
// Trampolines reached only from the LAN patch set (lan_patches.txt).
// ---------------------------------------------------------------------------------------------

// on_populate_friend(struct menu_friend_list_friend *friend, void *license, int index)
extern "C" void Lan_OnPopulateFriend_819f0000(CpuContext* ctx) {
    const int index = static_cast<int>(ctx->gpr[5]);
    if (index >= 0 && index < kMaxLanPeers) SetFriend(index, ctx->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x819F0000, Lan_OnPopulateFriend_819f0000, "Lan_OnPopulateFriend_819f0000");

// sleep_a_frame(): pace the net main thread to one frame of time base instead of the VI
// retrace ("high data rate").
extern "C" void Lan_SleepAFrame_819f0010(CpuContext* ctx) {
    static uint32_t lastTime = 0;
    constexpr uint32_t kSleepTarget = static_cast<uint32_t>(TimeBaseContract::kTicksPerSecond * 16666u / 1000000u);
    uint32_t thisTime = static_cast<uint32_t>(GuestTime(ctx));
    const uint32_t difference = thisTime - lastTime;
    if (difference < kSleepTarget) {
        const uint32_t sleep = kSleepTarget - difference;
        ctx->gpr[3] = 0;
        ctx->gpr[4] = sleep;
        InvokeIndirectCpu(kOSSleepTicks, ctx);
        thisTime += sleep;
    }
    lastTime = thisTime;
}
REGISTER_NATIVE_FUNCTION_AS(0x819F0010, Lan_SleepAFrame_819f0010, "Lan_SleepAFrame_819f0010");

// Mario Kart Channel pressed: explain that the channel is not available in LAN mode.
extern "C" void Lan_OnMarioKartChannelPressed_819f0020(CpuContext* ctx) {
    ShowPopup(ctx, MenuPanel(MenuData(), 0x5A), 11003, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x819F0020, Lan_OnMarioKartChannelPressed_819f0020,
                            "Lan_OnMarioKartChannelPressed_819f0020");

extern "C" void Lan_NetworkThreadMain_819f0030(CpuContext* ctx) {
    NetworkThreadMain(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x819F0030, Lan_NetworkThreadMain_819f0030, "Lan_NetworkThreadMain_819f0030");
