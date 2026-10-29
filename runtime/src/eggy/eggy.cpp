// Eggy's host API, C++ side (see runtime/include/eggy/eggy.h, web/js/library_eggy.js, EGGY.md).
//
// Everything here runs on the guest's own thread, from five hooks on the game's frame loop and
// race setup. JavaScript never calls guest code: the page writes mailboxes in the shared block
// (g_shared) and reads the snapshots this file writes at the end of every frame. The engine calls
// out to the page through the eggy_* imports, two of which suspend this stack (JSPI): eggy_wait
// while idle or parked, and eggy_pads while the lockstep has not got a frame's pads yet.
//
// The frame loop (RKSystem::run 0x8000951C) per frame: beginFrame; sceneMgr->draw (0x80009988);
// display endRender (0x8020FF9C, already an HLE in hle/gx/gx_egg.cpp, which asks SkipEndRender());
// sceneMgr->calc (0x80009984); endFrame. All game logic, and the
// one PADRead of a race frame (KPadDirector::calc from GameScene::calc), happens inside calc, so
// the hook on calc is the frame boundary: before it the engine takes the frame's pads or parks,
// after it the engine snapshots, hashes and reports.
//
// States: booting (the game is starting) -> idle (parked, nothing runs) -> loading (a section
// change to the race runs with neutral pads) -> parked (the race's first frame, nothing runs)
// -> racing (one calc per race frame, pads from the page) -> finished (report written) ->
// results (the config asked for N seconds of the game's own finish and results screens: frames
// run with neutral pads, nothing goes back to the page) -> idle.
//
// Pace: a race frame is due every 16.667 ms of wall time (divided by the lockstep's pace). A phone
// that falls a whole frame behind runs up to three more calcs before it draws again, so the race
// keeps the game's real speed and only the picture rate drops; past six frames behind it lets the
// debt go and runs slow instead of spiralling. drawEvery(n) draws one frame in n on purpose.

#include "eggy/eggy.h"

#include "abi_bridge.h"
#include "audio_backend.h"
#include "memory.h"
#include "music_attenuation.h"
#include "platform/web_host.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__EMSCRIPTEN__)
extern "C" {
int eggy_enabled(void);
void eggy_attach(int32_t* block, int bytes);
void eggy_wait(int ms);
int eggy_pads(int frame, uint8_t* dest);
void eggy_frame_done(int frame);
void eggy_state(int state);
void eggy_report_ready(void);
}
#else
namespace {
int eggy_enabled() { return 0; }
void eggy_attach(int32_t*, int) {}
void eggy_wait(int) {}
int eggy_pads(int, uint8_t*) { return 0; }
void eggy_frame_done(int) {}
void eggy_state(int) {}
void eggy_report_ready() {}
} // namespace
#endif

// The game's own functions the hooks wrap (translated bodies; the hooks below win the dispatch).
extern "C" void func_805302C4(CpuContext* ctx); // System::RaceConfig::initRace
extern "C" void func_8051AF84(CpuContext* ctx); // System::GameScene::setFramerate(bool is30)
extern "C" void func_80856278(CpuContext* ctx); // Pages::RaceHUD::OnInit
extern "C" void func_80856CEC(CpuContext* ctx); // Pages::RaceHUD::BeforeControlUpdate
extern "C" void func_808574B8(CpuContext* ctx); // Pages::RaceHUD::AfterControlUpdate
extern "C" void func_80858A50(CpuContext* ctx); // Pages::RaceHUD::OnItemObjTargeting
extern "C" void func_80858AE4(CpuContext* ctx); // Pages::RaceHUD::UpdateRaceBalloons

namespace {

// ---- Shared block (keep in step with web/js/library_eggy.js `$Eggy.W`) ----
constexpr int kMagic = 0, kVersion = 1, kState = 2, kFrame = 3, kHashFrame = 4, kHash = 5, kStage = 6,
              kCountdown = 7, kReportSeq = 8, kError = 9, kSection = 10, kTicks = 11, kLogicUs = 12,
              kDrawUs = 13, kPadCalls = 14, kRaceTimer = 15;
constexpr int kLoadSeq = 16, kGoSeq = 17, kAbortSeq = 18, kRenderSkip = 19, kMusic = 20, kSfx = 21,
              kInsets = 22, kPace = 26, kFinishAll = 27, kAway = 28, kDrawEvery = 32, kPacingOff = 33,
              kDebug = 34, kSlowDrawUs = 35;
constexpr int kConfig = 48, kConfigWords = 120, kPads = 168, kKarts = 176, kKartWords = 32, kHud = 304,
              kReport = 384, kDiag = 448, kPerf = 480, kPerfWords = 32, kWords = 512;
static_assert(kDiag + 32 <= kPerf && kPerf + kPerfWords <= kWords, "the diag and perf blocks overlap");

alignas(16) int32_t g_shared[kWords];

enum State : int32_t {
    kOff = 0,
    kBooting = 1,
    kIdle = 2,
    kLoading = 3,
    kParked = 4,
    kRacing = 5,
    kFinished = 6,
    kFailed = 7,
    kResults = 8,
};
const char* const kStateNames[] = {"off",      "booting", "idle",   "loading", "parked",
                                   "racing",   "finished", "failed", "results"};

// Errors (kError).
constexpr int32_t kErrBadConfig = 1, kErrNoSectionMgr = 2, kErrLoadTimeout = 3;

// ---- Config v1 (games/mkw/kit/config.ts in Eggy) ----
constexpr int kCfgVersion = 0, kCfgSeed = 1, kCfgGameMode = 2, kCfgBattleType = 3, kCfgEngineClass = 4,
              kCfgMirror = 5, kCfgItemMode = 6, kCfgCpuMode = 7, kCfgTeams = 8, kCfgCourse = 9, kCfgLaps = 10,
              kCfgField = 14, kCfgHumans = 15, kCfgMyPort = 16, kCfgResultsSeconds = 17, kCfgPlayers = 24,
              kCfgPlayerWords = 8;
constexpr int32_t kTypeHuman = 0, kTypeNone = 5;
constexpr int kGrid = 12;

// ---- Guest addresses (PAL RMCP01; verified by disassembly, see EGGY.md) ----
constexpr uint32_t kKPadDirector = 0x809BD70C;  // System::KPadDirector* (Input::Manager)
constexpr uint32_t kRaceConfig = 0x809BD728;    // System::RaceConfig*
constexpr uint32_t kRaceManager = 0x809BD730;   // RaceManager* (Raceinfo)
constexpr uint32_t kKartManager = 0x809C18F8;   // Kart::Manager*
constexpr uint32_t kCameraManager = 0x809C19B8; // Graphics::CameraManager*
constexpr uint32_t kSectionMgr = 0x809C1E38;    // UI::SectionManager*
constexpr uint32_t kItemManager = 0x809C3618;   // Item::Manager*
constexpr uint32_t kMtMaxAddr = 0x808B5CC2;     // s16 mini-turbo charge for blue sparks (270)
constexpr uint32_t kSmtMaxAddr = 0x808B5CC6;    // s16 super mini-turbo charge for orange (300)

constexpr uint32_t kSectionVs1P = 0x20, kSectionTeamVs1P = 0x24, kSectionBattle1P = 0x28;

// RaceConfig: race scenario at +0x20, menu scenario at +0xC10. Scenario: +4 playerCount, +5 hudCount,
// +6 localPlayerCount, +7 hudCount (second copy); players at +8 (0xF0 each); settings at +0xB48.
constexpr uint32_t kRaceScenario = 0x20, kMenuScenario = 0xC10, kScenarioPlayers = 8, kPlayerSize = 0xF0;
constexpr uint32_t kLocalPlayerCount = 6;

// RaceCameraMgr: +0x14 cameras (one per screen: camera i follows hudPlayerIds[i]; one per player only
// in live view), RaceCamera +0x9C playerId. Pages::RaceHUD instance: +0x60 hudHasPlayer[4].
constexpr uint32_t kRaceHud = 0x809C4680;

constexpr uint16_t kRaceButtons = 0x0F7F; // every GameCube button but Start
constexpr uint16_t kPadL = 0x0040, kPadR = 0x0020;

// ---- Guest memory helpers (never throw; 0 on an unmapped address) ----
bool Ok(uint32_t addr, size_t len) { return addr >= 0x80000000u && Memory::Contains(addr, len); }
uint32_t R32(uint32_t a) { return Ok(a, 4) ? Memory::Read32(a) : 0; }
uint16_t R16(uint32_t a) { return Ok(a, 2) ? Memory::Read16(a) : 0; }
uint8_t R8(uint32_t a) { return Ok(a, 1) ? Memory::Read8(a) : 0; }
float RF(uint32_t a) { return Ok(a, 4) ? Memory::ReadFloat32(a) : 0.0f; }
void W32(uint32_t a, uint32_t v) {
    if (Ok(a, 4)) Memory::Write32(a, v);
}
void W16(uint32_t a, uint16_t v) {
    if (Ok(a, 2)) Memory::Write16(a, v);
}
void W8(uint32_t a, uint8_t v) {
    if (Ok(a, 1)) Memory::Write8(a, v);
}
uint32_t Ptr(uint32_t a) {
    const uint32_t p = R32(a);
    return Ok(p, 4) ? p : 0;
}

void PutF32(int word, float v) { std::memcpy(&g_shared[word], &v, 4); }
float GetF32(int word) {
    float v;
    std::memcpy(&v, &g_shared[word], 4);
    return v;
}
// Mailboxes are written by JavaScript while this stack is suspended; read them fresh every time.
int32_t Mail(int word) { return reinterpret_cast<volatile int32_t*>(g_shared)[word]; }

// ---- State ----
int g_enabled = -1; // -1 not asked yet
State g_state = kOff;
int32_t g_seenLoad = 0, g_seenGo = 0, g_seenAbort = 0;
std::array<int32_t, kConfigWords> g_cfg{};
uint32_t g_targetSection = 0;
bool g_initRaceSeen = false;
int g_bootSettled = 0;
std::chrono::steady_clock::time_point g_loadStarted;

int g_frame = 0;        // the race frame the next calc runs (0 at go)
bool g_padsLive = false; // g_shared[kPads] holds the pads of the frame being calculated
std::array<int, 4> g_slotOfPort{-1, -1, -1, -1};
int g_mySlot = 0;
int g_humans = 0;
bool g_oneHudSlot = false; // the race's HUD page sees one local player (OneHudSlot, below)
int g_field = 0;
bool g_mirror = false;
bool g_battle = false;
std::array<uint32_t, 4> g_localHolder{}; // KPadDirector holder of local player k
std::array<uint32_t, 4> g_localPad{};    // GameCube controller it is bound to
std::array<int, 4> g_localSlot{-1, -1, -1, -1};
int g_localCount = 0;
std::array<bool, 4> g_awayApplied{};
int g_mtMax = 270, g_smtMax = 300;

int32_t g_music = 1, g_sfx = 1;
float g_pace = 1.0f;
bool g_skipThisDraw = false;
int g_resultsSeconds = 0; // config word 17: the game's own finish and results screens, 0 = none

// Timing: the calc and draw cost of the last 128 race frames, p95 published every 30 frames.
std::array<uint32_t, 128> g_logicUs{}, g_drawUs{};
uint32_t g_logicN = 0, g_drawN = 0;

uint32_t g_initRaceCalls = 0, g_framerateCalls = 0, g_rebinds = 0, g_drawsSkipped = 0;

using SteadyClock = std::chrono::steady_clock;

uint32_t ElapsedUs(SteadyClock::time_point since) {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - since).count());
}

int64_t Nanos(SteadyClock::duration d) { return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(); }

// ---- Pace: race frame g_frame is due at g_due (wall clock) ----
constexpr int kMaxExtraCalcs = 3; // calcs a late pass runs on top of its own
constexpr int kMaxDebtFrames = 6; // further behind than this, the debt is dropped
SteadyClock::time_point g_due;
int g_drawPass = 0; // passes of the frame loop since go (drawEvery counts these)
SteadyClock::time_point g_resultsStarted;
int g_resultsFrames = 0;

SteadyClock::duration FramePeriod() {
    return std::chrono::nanoseconds(static_cast<int64_t>(16666667.0 / static_cast<double>(g_pace)));
}

// ---- Perf: the breakdown of the last window (about a second), published at kPerf ----
struct PerfOpen {
    int part;
    SteadyClock::time_point start;
    int64_t childNs;
};
std::array<PerfOpen, 16> g_perfOpen{};
int g_perfDepth = 0;
std::array<int64_t, Eggy::Perf::kParts> g_perfNs{};
struct PerfWindow {
    SteadyClock::time_point start;
    SteadyClock::time_point lastPass;
    int32_t frames = 0, passes = 0, drawn = 0, late20 = 0, late34 = 0, extra = 0, dropped = 0, behindMax = 0;
};
PerfWindow g_perfWin;

bool PerfCounting() { return g_state == kRacing || g_state == kResults; }

void ResetPerfWindow(SteadyClock::time_point now) {
    g_perfWin = PerfWindow{};
    g_perfWin.start = now;
    g_perfWin.lastPass = now;
    g_perfNs.fill(0);
}

void PublishPerf(SteadyClock::time_point now) {
    int32_t* w = &g_shared[kPerf];
    w[0] = w[0] + 1;
    w[1] = static_cast<int32_t>(std::min<int64_t>(Nanos(now - g_perfWin.start) / 1000, INT32_MAX));
    w[2] = g_perfWin.frames;
    w[3] = g_perfWin.passes;
    w[4] = g_perfWin.drawn;
    for (int i = 0; i < Eggy::Perf::kParts; ++i) {
        w[5 + i] = static_cast<int32_t>(std::min<int64_t>(g_perfNs[i] / 1000, INT32_MAX));
    }
    w[13] = g_perfWin.late20;
    w[14] = g_perfWin.late34;
    w[15] = std::clamp(Mail(kDrawEvery), 1, 4);
    w[16] = g_perfWin.extra;
    w[17] = g_perfWin.dropped;
    w[18] = g_perfWin.behindMax;
    w[19] = Mail(kPacingOff) != 0 ? 0 : 1;
    ResetPerfWindow(now);
}

void SetState(State s) {
    if (g_state == s && s != kIdle) {
        return;
    }
    std::printf("[eggy] state %s -> %s (race frame %d)\n", kStateNames[g_state], kStateNames[s], g_frame);
    std::fflush(stdout);
    g_state = s;
    g_shared[kState] = s;
    eggy_state(s);
}

void Fail(int32_t error, const char* why) {
    g_shared[kError] = error;
    std::printf("[eggy] failed (%d): %s\n", error, why);
    SetState(kFailed);
}

// ---- Calling the game's own functions from a hook (on a copy of the caller's registers) ----
template <uint32_t Target>
uint32_t CallGuest(CpuContext* ctx, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    CpuContext tmp = *ctx;
    tmp.gpr[3] = r3;
    tmp.gpr[4] = r4;
    tmp.gpr[5] = r5;
    InvokeDirectCpu<Target>(&tmp);
    return tmp.gpr[3];
}

// ---- Waiting (the whole console parks: every guest thread is a context on this stack) ----
void ParkUntilMailbox() {
    const auto start = SteadyClock::now();
    AudioBackend::Instance().SetOutputPaused(true);
    eggy_wait(0);
    AudioBackend::Instance().SetOutputPaused(false);
    const auto parked = std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - start);
    WebHost::ParkedNanosCounter().fetch_add(parked.count(), std::memory_order_relaxed);
    VI_HLE_EggyNoteHostPark(parked);
}

bool LoadPending() { return Mail(kLoadSeq) != g_seenLoad; }
bool GoPending() { return Mail(kGoSeq) != g_seenGo; }
bool AbortPending() { return Mail(kAbortSeq) != g_seenAbort; }

// ---- Mailboxes that apply at any time ----
void ApplyMailboxes() {
    const int32_t music = Mail(kMusic) != 0 ? 1 : 0;
    const int32_t sfx = Mail(kSfx) != 0 ? 1 : 0;
    if (music != g_music) {
        g_music = music;
        MusicAttenuation::SetMusicVolume(music ? 1.0f : 0.0f);
    }
    if (sfx != g_sfx) {
        g_sfx = sfx;
        const float v = sfx ? 1.0f : 0.0f;
        MusicAttenuation::SetSoundEffectsVolume(v);
        MusicAttenuation::SetUiVolume(v);
        MusicAttenuation::SetVoicesVolume(v);
    }
    float pace = GetF32(kPace);
    if (!(pace >= 0.9f && pace <= 1.1f)) {
        pace = 1.0f;
    }
    if (pace != g_pace) {
        g_pace = pace;
        VI_HLE_EggySetPace(pace);
    }
    // TODO(eggy): hudInsets (kInsets..kInsets+3, CSS px of the stage) are stored but not applied:
    // the race HUD's layout (RaceHUD page, LYT panes) does not move yet.
}

// ---- Game state readers ----
uint32_t SectionMgr() { return Ptr(kSectionMgr); }
uint32_t CurrentSectionId() {
    const uint32_t sm = SectionMgr();
    const uint32_t sec = sm ? Ptr(sm) : 0;
    return sec ? R32(sec) : 0xFFFFFFFFu;
}
uint32_t RaceManager() { return Ptr(kRaceManager); }
uint32_t RacePlayer(int slot) {
    const uint32_t rm = RaceManager();
    const uint32_t players = rm ? Ptr(rm + 0xC) : 0;
    return players ? Ptr(players + static_cast<uint32_t>(slot) * 4) : 0;
}
int KartCount() {
    const uint32_t km = Ptr(kKartManager);
    return km ? R8(km + 0x24) : 0;
}
uint32_t KartPlayer(int slot) {
    const uint32_t km = Ptr(kKartManager);
    if (!km || slot < 0 || slot >= R8(km + 0x24)) return 0;
    const uint32_t arr = Ptr(km + 0x20);
    return arr ? Ptr(arr + static_cast<uint32_t>(slot) * 4) : 0;
}
uint32_t KartPointers(int slot) {
    const uint32_t kart = KartPlayer(slot);
    return kart ? Ptr(kart) : 0;
}
uint32_t ItemPlayer(int slot) {
    const uint32_t im = Ptr(kItemManager);
    const uint32_t players = im ? Ptr(im + 0x14) : 0;
    return players ? players + static_cast<uint32_t>(slot) * 0x248 : 0;
}
int PlayerCount() {
    const uint32_t rc = Ptr(kRaceConfig);
    return rc ? R8(rc + kRaceScenario + 4) : 0;
}
int Stage() {
    const uint32_t rm = RaceManager();
    return rm ? static_cast<int>(R32(rm + 0x28)) : -1;
}
int RaceFrames() {
    const uint32_t rm = RaceManager();
    return rm ? static_cast<int>(R32(rm + 0x20)) : 0;
}
// The player race camera `i` follows (-1 when there is no such camera).
int CameraPlayer(uint32_t cm, int i) {
    if (!cm || i < 0 || static_cast<uint32_t>(i) >= R32(cm + 0x1C)) return -1;
    const uint32_t cameras = Ptr(cm + 0x14);
    const uint32_t camera = cameras ? Ptr(cameras + static_cast<uint32_t>(i) * 4) : 0;
    return camera ? R8(camera + 0x9C) : -1;
}

// ---- Binding a local player to a GameCube port ----
uint32_t GcnPad(int port) {
    const uint32_t kp = Ptr(kKPadDirector);
    return kp ? kp + 0x3BA0 + static_cast<uint32_t>(port) * 0xB0 : 0;
}
uint32_t LocalHolder(int k) {
    const uint32_t kp = Ptr(kKPadDirector);
    return kp ? kp + 4 + static_cast<uint32_t>(k) * 0xEC : 0;
}

void BindLocal(CpuContext* ctx, int k, int port, bool autoDrift) {
    const uint32_t holder = LocalHolder(k);
    const uint32_t pad = GcnPad(port);
    if (!holder || !pad) return;
    if (R32(holder + 4) != pad || R32(holder + 8) != pad) {
        CallGuest<0x80521554>(ctx, holder, pad, 0); // System::PadProxy::setPad(pad, ghost = none)
    }
    CallGuest<0x80520F2C>(ctx, holder, autoDrift ? 1u : 0u); // KPad::setDriftIsAuto
    // The section's pad record for HUD slot k: connected, this holder, stored ID == active ID, so
    // no "controller changed / disconnected" check ever trips.
    const uint32_t sm = SectionMgr();
    if (sm && k < 4) {
        const uint32_t id = CallGuest<0x8061BE40>(ctx, holder); // SectionPad::CalculateID(holder)
        const uint32_t info = sm + 0x34 + 4 + static_cast<uint32_t>(k) * 0x10;
        W32(info + 0x0, id);
        W32(info + 0x4, id);
        W32(info + 0x8, 0);
        W32(info + 0xC, holder);
    }
    g_localHolder[k] = holder;
    g_localPad[k] = pad;
}

void RebindIfMoved(CpuContext* ctx) {
    for (int k = 0; k < g_localCount; ++k) {
        const uint32_t holder = g_localHolder[k];
        if (holder && R32(holder + 4) != g_localPad[k]) {
            const int slot = g_localSlot[k];
            const int at = kCfgPlayers + slot * kCfgPlayerWords;
            std::printf("[eggy] local %d (slot %d) lost its pad (0x%08x), rebinding\n", k, slot, R32(holder + 4));
            BindLocal(ctx, k, g_cfg[at + 1], g_cfg[at + 4] == 1);
            ++g_rebinds;
        }
    }
}

// ---- Load ----
bool ReadConfig() {
    for (int i = 0; i < kConfigWords; ++i) g_cfg[i] = Mail(kConfig + i);
    if (g_cfg[kCfgVersion] != 1) return false;
    const int mode = g_cfg[kCfgGameMode];
    if (mode != 1 && mode != 3) return false;
    g_field = std::clamp(g_cfg[kCfgField], 1, kGrid);
    g_resultsSeconds = std::clamp(g_cfg[kCfgResultsSeconds], 0, 30);
    g_battle = mode == 3;
    g_mirror = g_cfg[kCfgMirror] == 1;
    g_slotOfPort = {-1, -1, -1, -1};
    g_localSlot = {-1, -1, -1, -1};
    g_humans = 0;
    for (int slot = 0; slot < g_field; ++slot) {
        const int at = kCfgPlayers + slot * kCfgPlayerWords;
        if (g_cfg[at] != kTypeHuman) continue;
        const int port = g_cfg[at + 1];
        if (port < 0 || port > 3 || g_slotOfPort[port] >= 0 || g_humans >= 4) return false;
        g_slotOfPort[port] = slot;
        g_localSlot[g_humans] = slot;
        ++g_humans;
    }
    g_localCount = g_humans;
    const int myPort = g_cfg[kCfgMyPort];
    g_mySlot = (myPort >= 0 && myPort < 4 && g_slotOfPort[myPort] >= 0) ? g_slotOfPort[myPort]
               : (g_humans > 0 ? g_localSlot[0] : 0);
    return true;
}

bool StartLoad() {
    g_seenLoad = Mail(kLoadSeq);
    g_seenGo = Mail(kGoSeq);
    g_seenAbort = Mail(kAbortSeq);
    g_shared[kError] = 0;
    if (!ReadConfig()) {
        Fail(kErrBadConfig, "config is not a valid v1 race config");
        return false;
    }
    const uint32_t sm = SectionMgr();
    if (!sm) {
        Fail(kErrNoSectionMgr, "no SectionManager");
        return false;
    }
    g_targetSection = g_battle ? kSectionBattle1P : (g_cfg[kCfgTeams] == 1 ? kSectionTeamVs1P : kSectionVs1P);
    // SectionManager::setNextSection without its priority check, then startChangeSection(0, black).
    W32(sm + 0x0C, g_targetSection);
    W32(sm + 0x18, 0);
    if (R32(sm + 0x30) == 0) {
        W32(sm + 0x30, 2);
        W32(sm + 0x1C, 0);
        W32(sm + 0x28, 0x000000FF);
    } else {
        std::printf("[eggy] section change while fade action %u is pending\n", R32(sm + 0x30));
        W32(sm + 0x30, 2);
    }
    g_oneHudSlot = g_humans > 1;
    g_initRaceSeen = false;
    g_frame = 0;
    g_padsLive = false;
    g_awayApplied = {};
    g_shared[kFrame] = 0;
    g_shared[kHash] = 0;
    g_shared[kHashFrame] = -1;
    g_shared[kStage] = -1;
    g_shared[kCountdown] = -1;
    for (int i = kKarts; i < kKarts + 4 * kKartWords; ++i) g_shared[i] = 0;
    g_shared[kHud] = -1;
    g_shared[kHud + 1] = 0;
    g_logicN = g_drawN = 0;
    g_shared[kLogicUs] = g_shared[kDrawUs] = 0;
    g_loadStarted = SteadyClock::now();
    std::printf("[eggy] load: section 0x%02x -> 0x%02x course 0x%02x mode %d cc %d mirror %d items %d cpus %d "
                "field %d humans %d myPort %d (slot %d) laps %d seed 0x%08x\n",
                CurrentSectionId(), g_targetSection, g_cfg[kCfgCourse], g_cfg[kCfgGameMode], g_cfg[kCfgEngineClass],
                g_cfg[kCfgMirror], g_cfg[kCfgItemMode], g_cfg[kCfgCpuMode], g_field, g_humans, g_cfg[kCfgMyPort],
                g_mySlot, g_cfg[kCfgLaps], static_cast<uint32_t>(g_cfg[kCfgSeed]));
    return true;
}

// Writes the config into the menu scenario, which RaceConfig::initRace turns into the race.
void WriteMenuScenario(uint32_t rc) {
    const uint32_t menu = rc + kMenuScenario;
    for (int slot = 0; slot < kGrid; ++slot) {
        const uint32_t p = menu + kScenarioPlayers + static_cast<uint32_t>(slot) * kPlayerSize;
        const int at = kCfgPlayers + slot * kCfgPlayerWords;
        const int32_t type = slot < g_field ? g_cfg[at] : kTypeNone;
        W32(p + 0x10, static_cast<uint32_t>(type));
        if (type != kTypeNone) {
            W32(p + 0x0C, static_cast<uint32_t>(g_cfg[at + 2])); // character
            W32(p + 0x08, static_cast<uint32_t>(g_cfg[at + 3])); // vehicle
        }
        W32(p + 0xCC, static_cast<uint32_t>(g_cfg[at + 5])); // team
        const uint16_t points = static_cast<uint16_t>(std::max(0, g_cfg[at + 6]));
        W16(p + 0xD8, points); // previous score
        W16(p + 0xDA, points); // score
    }
    W32(menu + 0xB48, static_cast<uint32_t>(g_cfg[kCfgCourse]));
    W32(menu + 0xB4C, static_cast<uint32_t>(g_cfg[kCfgEngineClass]));
    W32(menu + 0xB50, static_cast<uint32_t>(g_cfg[kCfgGameMode]));
    W32(menu + 0xB54, 0); // camera mode 0: no skippable intro, countdown once the camera is ready
    W32(menu + 0xB58, static_cast<uint32_t>(g_cfg[kCfgBattleType]));
    W32(menu + 0xB5C, static_cast<uint32_t>(g_cfg[kCfgCpuMode]));
    W32(menu + 0xB60, static_cast<uint32_t>(g_cfg[kCfgItemMode]));
    W8(menu + 0xB6C, 0); // race number: the grid is the slot order
    const int laps = g_cfg[kCfgLaps];
    W8(menu + 0xB6D, static_cast<uint8_t>(laps > 0 && laps < 10 ? laps : 3));
    uint32_t flags = 0;
    if (g_mirror) flags |= 1;
    if (g_cfg[kCfgTeams] == 1) flags |= 2;
    W32(menu + 0xB70, flags);
}

void WriteSeeds(uint32_t scenario) {
    const uint32_t seed = static_cast<uint32_t>(g_cfg[kCfgSeed]);
    W32(scenario + 0xB74, seed);
    W32(scenario + 0xB78, seed ^ 0x9E3779B9u);
}

// One screen whatever the number of humans: camera, HUD and sound listener follow this phone's
// player only. The others stay local players (their pads drive their karts) without a screen of
// their own. This is the shape Scenario::initRace (0x8052FB90) itself gives the VS intro (camera
// mode 5): screen count (+5) 1 while every local player keeps a local number (player +5) and a
// hudPlayerIds entry. Here this phone's player is local 0 (the one screen), the other humans 1..
// in slot order.
//
// What reads which (disassembly of RMCP01, EGGY.md):
// - The screen count sizes the cameras (RaceCameraMgr ctor: camera i is built for
//   GetPlayerIdOfLocalPlayer(i) = hudPlayerIds[i]), the audio listeners (Audio::RaceMgr) and the
//   Lakitus, so one camera, one listener and one Lakitu follow hudPlayerIds[0] natively.
// - localPlayerCount (+6) stays the number of humans: the race needs it. The AI walks that many
//   real players (AI::Manager::GetLocalPlayerCount, list filled by type in slot order) and course
//   objects size per-local-kart buffers by it (e.g. 0x80753B34 writes one entry per REAL_LOCAL kart).
// - Pages::RaceHUD lays out and loops over its HUD slots by localPlayerCount, not by screens; the
//   OneHudSlot hooks below show it one.
// - Local numbers and hudPlayerIds reach nothing that changes the race: kart "local" status,
//   race end (GMDataVS::CanRaceEnd), items (DriverMgr::IsPlayerComputer) and the AI's real players
//   all go by player type; the rest is camera, HUD, sound, effects and models.
// RaceCameraMgr::ChangeFocusedPlayer (0x805A9B04) is live view's switch: it indexes cameras by
// player id, which only holds when live view built one camera per player. Never call it here.
void PatchOneScreen(uint32_t rc) {
    const uint32_t race = rc + kRaceScenario;
    if (g_humans <= 1 && R8(race + 0xB64) == static_cast<uint8_t>(g_mySlot)) {
        return;
    }
    W8(race + 5, 1);
    W8(race + 7, 1);
    for (int i = 0; i < 4; ++i) W8(race + 0xB64 + static_cast<uint32_t>(i), 0xFF);
    W8(race + 0xB64, static_cast<uint8_t>(g_mySlot));
    int next = 1;
    for (int k = 0; k < g_localCount; ++k) {
        const int slot = g_localSlot[k];
        const uint32_t p = race + kScenarioPlayers + static_cast<uint32_t>(slot) * kPlayerSize;
        if (slot == g_mySlot) {
            W8(p + 5, 0);
            continue;
        }
        W8(p + 5, static_cast<uint8_t>(next));
        W8(race + 0xB64 + static_cast<uint32_t>(next), static_cast<uint8_t>(slot));
        ++next;
    }
}

void LogScenario(uint32_t rc, const char* what) {
    const uint32_t race = rc + kRaceScenario;
    std::printf("[eggy] %s: players %u hud %u local %u hud2 %u hudIds %d %d %d %d course 0x%02x mode %u "
                "class %u camera %u laps %u flags 0x%x seeds 0x%08x 0x%08x\n",
                what, R8(race + 4), R8(race + 5), R8(race + 6), R8(race + 7),
                static_cast<int8_t>(R8(race + 0xB64)), static_cast<int8_t>(R8(race + 0xB65)),
                static_cast<int8_t>(R8(race + 0xB66)), static_cast<int8_t>(R8(race + 0xB67)), R32(race + 0xB48),
                R32(race + 0xB50), R32(race + 0xB4C), R32(race + 0xB54), R8(race + 0xB6D), R32(race + 0xB70),
                R32(race + 0xB74), R32(race + 0xB78));
    for (int slot = 0; slot < g_field; ++slot) {
        const uint32_t p = race + kScenarioPlayers + static_cast<uint32_t>(slot) * kPlayerSize;
        std::printf("[eggy]   slot %d type %u char 0x%02x vehicle 0x%02x local %d input %d controller %d team %u\n",
                    slot, R32(p + 0x10), R32(p + 0xC), R32(p + 8), static_cast<int8_t>(R8(p + 5)),
                    static_cast<int8_t>(R8(p + 6)), static_cast<int32_t>(R32(p + 0xD0)), R32(p + 0xCC));
    }
}

// Every couple of seconds while booting or loading: where the game is, for the smoke logs.
void LogProgress(const char* what) {
    const uint32_t sm = SectionMgr();
    std::printf("[eggy] %s: tick %d section 0x%02x fade %d initRace %d raceManager 0x%08x stage %d karts %d "
                "pad calls %d\n",
                what, g_shared[kTicks], CurrentSectionId(), sm ? static_cast<int>(R32(sm + 0x30)) : -1,
                g_initRaceSeen ? 1 : 0, RaceManager(), Stage(), KartCount(), g_shared[kPadCalls]);
}

bool LoadReady() {
    if (!g_initRaceSeen) return false;
    const uint32_t sm = SectionMgr();
    if (!sm || CurrentSectionId() != g_targetSection || R32(sm + 0x30) != 0) return false;
    if (!RaceManager() || KartCount() < g_field) return false;
    return true;
}

// ---- Snapshots, hash, report ----
void SnapshotKart(int port, int stage, int raceFrames) {
    int32_t* w = &g_shared[kKarts + port * kKartWords];
    std::fill(w, w + kKartWords, 0);
    w[16] = -1;
    const int slot = g_slotOfPort[port];
    if (slot < 0) return;
    const uint32_t ptrs = KartPointers(slot);
    const uint32_t values = ptrs ? Ptr(ptrs + 0x0) : 0;
    const uint32_t status = ptrs ? Ptr(ptrs + 0x4) : 0;
    const uint32_t move = ptrs ? Ptr(ptrs + 0x28) : 0;
    if (!status || !move) return;
    const bool bike = values && R32(values) != 0;
    const uint32_t bf0 = R32(status + 0x4), bf1 = R32(status + 0x8), bf2 = R32(status + 0xC);
    const bool drifting = (bf0 & (0x8u | 0x10000000u)) != 0;
    const int hop = static_cast<int32_t>(R32(move + 0xCC));
    int dir = hop > 0 ? 1 : (hop < 0 ? -1 : 0);
    if (g_mirror) dir = -dir; // the game flips the stick in mirror mode; report the player's side
    w[0] = 1;
    w[1] = slot;
    PutF32(kKarts + port * kKartWords + 2, RF(move + 0x20)); // engine speed
    PutF32(kKarts + port * kKartWords + 3, RF(move + 0x14)); // base speed (the vehicle's top speed)
    w[4] = static_cast<int16_t>(R16(move + 0xFE));
    w[5] = g_mtMax;
    w[6] = static_cast<int16_t>(R16(move + 0x100));
    w[7] = bike ? 0 : g_smtMax;
    w[8] = drifting ? 1 : 0;
    w[9] = drifting ? dir : 0;
    w[10] = (bf0 & 0x40000u) ? 0 : 1;
    w[11] = ((bf1 & 0x40000000u) != 0 || static_cast<int16_t>(R16(status + 0xA6)) > 0) ? 1 : 0;
    w[12] = bike ? 1 : 0;
    w[13] = (bf0 & 0x20000000u) ? 1 : 0;
    if (stage == 1) {
        PutF32(kKarts + port * kKartWords + 14, RF(status + 0x9C));
        w[15] = 1;
        w[16] = std::max(0, 240 - raceFrames);
    }
    w[17] = ((bf0 & 0x10u) != 0 || R16(move + 0x234) != 0) ? 1 : 0;
    const uint32_t rp = RacePlayer(slot);
    if (rp) {
        w[18] = (R32(rp + 0x38) & 2u) ? 1 : 0;
        w[19] = R8(rp + 0x20);
        w[20] = R16(rp + 0x24);
    }
    // Debug words (not part of the API): status bitfields, drift state, input holder.
    w[21] = static_cast<int32_t>(bf0);
    w[22] = static_cast<int32_t>(bf1);
    w[23] = static_cast<int32_t>(bf2);
    w[24] = static_cast<int32_t>(R32(status + 0x14));
    w[25] = static_cast<int16_t>(R16(move + 0xFC));
    w[26] = rp ? static_cast<int32_t>(R32(rp + 0x48)) : 0;
}

void SnapshotHud(int frame) {
    const int count = std::min(PlayerCount(), kGrid);
    g_shared[kHud] = frame;
    g_shared[kHud + 1] = count;
    for (int slot = 0; slot < count; ++slot) {
        int32_t* l = &g_shared[kHud + 2 + slot * 6];
        const uint32_t rp = RacePlayer(slot);
        const uint32_t ip = ItemPlayer(slot);
        l[0] = slot;
        l[1] = rp ? R8(rp + 0x20) : 0;
        l[2] = rp ? R16(rp + 0x24) : 0;
        l[3] = (rp && g_battle) ? static_cast<int16_t>(R16(rp + 0x22)) : 0;
        l[4] = (ip && R32(ip + 0x90) != 0) ? static_cast<int32_t>(R32(ip + 0x8C)) : -1;
        l[5] = (rp && (R32(rp + 0x38) & 2u)) ? 1 : 0;
    }
}

struct Fnv {
    uint32_t h = 0x811C9DC5u;
    void Word(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 0x01000193u;
        }
    }
    void Words(uint32_t addr, int n) {
        for (int i = 0; i < n; ++i) Word(R32(addr + static_cast<uint32_t>(i) * 4));
    }
};

struct LineBuf {
    char text[4096];
    size_t at = 0;
    __attribute__((format(printf, 2, 3))) void Put(const char* fmt, ...) {
        if (at >= sizeof text) return;
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(text + at, sizeof text - at, fmt, args);
        va_end(args);
        if (n > 0) at += static_cast<size_t>(n);
    }
};

// Test only (the debug mailbox's bit 0, eggy.html ?hashParts=1): HashRace's parts one by one, so
// two runs whose hashes differ show which part they differ on. Physics is hashed, the rest raw.
void LogHashParts(int frame) {
    static LineBuf line;
    line.at = 0;
    auto hashed = [](uint32_t addr, int n) {
        Fnv f;
        f.Words(addr, n);
        return f.h;
    };
    line.Put("[eggy] hash parts frame %d:", frame);
    const uint32_t rm = RaceManager();
    if (rm) {
        line.Put(" rm %08x %08x", R32(rm + 0x28), R32(rm + 0x20));
        for (uint32_t off : {0x4u, 0x8u}) {
            const uint32_t random = Ptr(rm + off);
            line.Put(" rng%u %08x", off / 4, random ? hashed(random + 0x8, 4) : 0u);
        }
    }
    const int karts = KartCount();
    for (int slot = 0; slot < karts; ++slot) {
        line.Put(" | k%d", slot);
        const uint32_t ptrs = KartPointers(slot);
        if (!ptrs) continue;
        const uint32_t status = Ptr(ptrs + 0x4);
        const uint32_t body = Ptr(ptrs + 0x8);
        const uint32_t holder = body ? Ptr(body + 0x90) : 0;
        const uint32_t phys = holder ? Ptr(holder + 0x4) : 0;
        const uint32_t move = Ptr(ptrs + 0x28);
        if (phys) line.Put(" pos %08x vel %08x rot %08x", hashed(phys + 0x68, 3), hashed(phys + 0xD4, 3), hashed(phys + 0xF0, 4));
        if (move) line.Put(" move %08x %08x %08x", R32(move + 0x20), R32(move + 0xFC), R32(move + 0x100));
        if (status) line.Put(" status %08x %08x %08x", R32(status + 0x4), R32(status + 0x8), R32(status + 0xC));
        const uint32_t rp = RacePlayer(slot);
        if (rp) line.Put(" player %08x %08x %08x %08x", R32(rp + 0xC), R32(rp + 0x20), R32(rp + 0x24), R32(rp + 0x38));
        const uint32_t ip = ItemPlayer(slot);
        if (ip) line.Put(" item %08x %08x", R32(ip + 0x8C), R32(ip + 0x90));
    }
    std::printf("%s\n", line.text);
}

// The race state that has to agree between phones in lockstep: every kart's physics and drive
// state, every racer's progress, the items held and the race's random generator. Only the first
// (RaceManager +0x4): drawing takes numbers from the second (+0x8), so it moves with what each
// phone draws, and nothing the race plays reads it (scrambled on every frame for 1500 frames, six
// karts with items, not one other part moved; the first scrambled the same way, the race parted
// within 240 frames).
uint32_t HashRace(int frame) {
    Fnv f;
    f.Word(static_cast<uint32_t>(frame));
    const uint32_t rm = RaceManager();
    f.Word(rm ? R32(rm + 0x28) : 0xFFFFFFFFu);
    f.Word(rm ? R32(rm + 0x20) : 0);
    const int karts = KartCount();
    for (int slot = 0; slot < karts; ++slot) {
        const uint32_t ptrs = KartPointers(slot);
        if (!ptrs) {
            f.Word(0xDEADu);
            continue;
        }
        const uint32_t status = Ptr(ptrs + 0x4);
        const uint32_t body = Ptr(ptrs + 0x8);
        const uint32_t holder = body ? Ptr(body + 0x90) : 0;
        const uint32_t phys = holder ? Ptr(holder + 0x4) : 0;
        const uint32_t move = Ptr(ptrs + 0x28);
        if (phys) {
            f.Words(phys + 0x68, 3); // position
            f.Words(phys + 0xD4, 3); // velocity
            f.Words(phys + 0xF0, 4); // rotation
        }
        if (move) {
            f.Word(R32(move + 0x20));  // engine speed
            f.Word(R32(move + 0xFC));  // drift state, mini-turbo charge
            f.Word(R32(move + 0x100)); // super mini-turbo charge
        }
        if (status) f.Words(status + 0x4, 3); // bitfields 0-2
        const uint32_t rp = RacePlayer(slot);
        if (rp) {
            f.Word(R32(rp + 0xC));  // race completion
            f.Word(R32(rp + 0x20)); // position, respawn, battle score
            f.Word(R32(rp + 0x24)); // lap
            f.Word(R32(rp + 0x38)); // state flags
        }
        const uint32_t ip = ItemPlayer(slot);
        if (ip) {
            f.Word(R32(ip + 0x8C));
            f.Word(R32(ip + 0x90));
        }
    }
    if (const uint32_t random = rm ? Ptr(rm + 0x4) : 0) f.Words(random + 0x8, 4);
    if ((Mail(kDebug) & 1) != 0) LogHashParts(frame);
    return f.h;
}

void WriteReport(int endFrame, uint32_t hash, const char* why) {
    const int count = std::min(PlayerCount(), kGrid);
    g_shared[kReport] = endFrame;
    g_shared[kReport + 1] = static_cast<int32_t>(hash);
    g_shared[kReport + 2] = count;
    std::printf("[eggy] report (%s) at race frame %d hash 0x%08x:\n", why, endFrame, hash);
    for (int slot = 0; slot < count; ++slot) {
        int32_t* l = &g_shared[kReport + 3 + slot * 4];
        const uint32_t rp = RacePlayer(slot);
        const bool finished = rp && (R32(rp + 0x38) & 2u);
        int32_t ms = -1;
        if (finished) {
            const uint32_t timer = Ptr(rp + 0x40);
            if (timer) {
                ms = static_cast<int32_t>(R16(timer + 4)) * 60000 + static_cast<int32_t>(R8(timer + 6)) * 1000 +
                     static_cast<int32_t>(R16(timer + 8));
            }
        }
        l[0] = slot;
        l[1] = rp ? R8(rp + 0x20) : 0;
        l[2] = ms;
        l[3] = (rp && g_battle) ? static_cast<int16_t>(R16(rp + 0x22)) : 0;
        std::printf("[eggy]   slot %d place %d finishMs %d score %d\n", l[0], l[1], l[2], l[3]);
    }
    g_shared[kReportSeq] = g_shared[kReportSeq] + 1;
}

// Hands `port`'s kart to the computer the way the game does when a racer finishes
// (RaceinfoPlayer::EndRace 0x805347F4): its input holder becomes the AI holder of its slot, and
// the kart is marked computer-driven (0x80590110).
void ApplyAway(CpuContext* ctx, int port) {
    const int slot = g_slotOfPort[port];
    const uint32_t rp = slot >= 0 ? RacePlayer(slot) : 0;
    const uint32_t kp = Ptr(kKPadDirector);
    if (!rp || !kp) return;
    W32(rp + 0x48, kp + 0x3B4 + static_cast<uint32_t>(slot) * 0x180);
    CallGuest<0x80590110>(ctx, static_cast<uint32_t>(slot));
    g_awayApplied[port] = true;
    std::printf("[eggy] port %d (slot %d) handed to the computer after race frame %d\n", port, slot, g_frame);
}

void PublishTiming() {
    auto p95 = [](const std::array<uint32_t, 128>& ring, uint32_t n) -> int32_t {
        const uint32_t count = std::min<uint32_t>(n, 128);
        if (count == 0) return 0;
        std::array<uint32_t, 128> copy = ring;
        const uint32_t k = (count * 95) / 100;
        std::nth_element(copy.begin(), copy.begin() + k, copy.begin() + count);
        return static_cast<int32_t>(copy[k]);
    };
    g_shared[kLogicUs] = std::max(1, p95(g_logicUs, g_logicN));
    g_shared[kDrawUs] = p95(g_drawUs, g_drawN);
}

void WriteDiag() {
    int32_t* d = &g_shared[kDiag];
    const uint32_t sm = SectionMgr();
    const uint32_t rc = Ptr(kRaceConfig);
    const uint32_t cm = Ptr(kCameraManager);
    d[0] = static_cast<int32_t>(CurrentSectionId());
    d[1] = sm ? static_cast<int32_t>(R32(sm + 0x30)) : -1;
    d[2] = static_cast<int32_t>(RaceManager());
    d[3] = Stage();
    d[4] = RaceFrames();
    d[5] = KartCount();
    d[6] = rc ? R8(rc + kRaceScenario + 4) : -1;
    d[7] = rc ? R8(rc + kRaceScenario + 5) : -1;
    d[8] = rc ? R8(rc + kRaceScenario + 6) : -1;
    for (int i = 0; i < 4; ++i) d[9 + i] = rc ? static_cast<int8_t>(R8(rc + kRaceScenario + 0xB64 + i)) : -2;
    d[13] = CameraPlayer(cm, 0);
    d[14] = static_cast<int32_t>(g_localHolder[0] ? R32(g_localHolder[0] + 4) : 0);
    d[15] = static_cast<int32_t>(g_localPad[0]);
    d[16] = static_cast<int32_t>(g_initRaceCalls);
    d[17] = static_cast<int32_t>(g_framerateCalls);
    d[18] = static_cast<int32_t>(g_rebinds);
    d[19] = static_cast<int32_t>(g_drawsSkipped);
    d[20] = g_mySlot;
    d[21] = g_humans;
    d[22] = static_cast<int32_t>(g_targetSection);
}

void OnParked() {
    g_mtMax = std::max<int>(1, static_cast<int16_t>(R16(kMtMaxAddr)));
    g_smtMax = std::max<int>(1, static_cast<int16_t>(R16(kSmtMaxAddr)));
    const uint32_t cm = Ptr(kCameraManager);
    const uint32_t hud = Ptr(kRaceHud);
    std::printf("[eggy] screen: cameras %d, camera 0 follows slot %d (mine %d); HUD slots %d %d %d %d\n",
                cm ? static_cast<int>(R32(cm + 0x1C)) : -1, CameraPlayer(cm, 0), g_mySlot,
                hud ? R8(hud + 0x60) : -1, hud ? R8(hud + 0x61) : -1, hud ? R8(hud + 0x62) : -1,
                hud ? R8(hud + 0x63) : -1);
    const int stage = Stage();
    for (int port = 0; port < 4; ++port) SnapshotKart(port, stage, RaceFrames());
    g_shared[kStage] = stage;
    WriteDiag();
    const uint32_t rc = Ptr(kRaceConfig);
    if (rc) LogScenario(rc, "parked");
    std::printf("[eggy] parked after %u ms: section 0x%02x stage %d raceFrames %d karts %d mtMax %d smtMax %d\n",
                ElapsedUs(g_loadStarted) / 1000, CurrentSectionId(), stage, RaceFrames(), KartCount(), g_mtMax,
                g_smtMax);
}

// ---- Before and after each calc ----
void PreCalc(CpuContext* ctx) {
    ++g_shared[kTicks];
    ApplyMailboxes();
    for (;;) {
        switch (g_state) {
        case kBooting: {
            const uint32_t sm = SectionMgr();
            const bool settled = sm && Ptr(sm) && R32(sm + 0x30) == 0;
            g_bootSettled = settled ? g_bootSettled + 1 : 0;
            if (g_bootSettled < 2) {
                if ((g_shared[kTicks] % 120) == 1) LogProgress("booting");
                return;
            }
            std::printf("[eggy] booted to section 0x%02x\n", CurrentSectionId());
            SetState(kIdle);
            continue;
        }
        case kFinished:
            if (g_resultsSeconds > 0 && !LoadPending() && !AbortPending()) {
                g_resultsStarted = SteadyClock::now();
                g_resultsFrames = 0;
                SetState(kResults);
                return; // this frame already runs the results
            }
            SetState(kIdle);
            continue;
        case kResults: {
            // The game's own finish and results screens, with neutral pads, until the config's
            // seconds are up; then idle parks on the last picture. Nothing here reaches the page.
            if (LoadPending()) {
                SetState(kIdle);
                continue;
            }
            if (AbortPending()) {
                g_seenAbort = Mail(kAbortSeq);
                SetState(kIdle);
                continue;
            }
            const uint32_t shownMs = ElapsedUs(g_resultsStarted) / 1000;
            if (shownMs >= static_cast<uint32_t>(g_resultsSeconds) * 1000u) {
                std::printf("[eggy] results shown for %u ms (%d frames)\n", shownMs, g_resultsFrames);
                SetState(kIdle);
                continue;
            }
            ++g_resultsFrames;
            return;
        }
        case kFailed:
            if (!LoadPending()) return;
            SetState(kIdle);
            continue;
        case kIdle:
            g_shared[kSection] = static_cast<int32_t>(CurrentSectionId());
            if (LoadPending()) {
                if (StartLoad()) {
                    SetState(kLoading);
                    return;
                }
                continue; // failed; a later load may be valid
            }
            if (AbortPending()) {
                g_seenAbort = Mail(kAbortSeq);
                eggy_state(kIdle); // already idle: tell the page so its abort() settles
            }
            ParkUntilMailbox();
            continue;
        case kLoading:
            if (LoadReady()) {
                OnParked();
                SetState(kParked);
                continue;
            }
            if ((g_shared[kTicks] % 120) == 0) LogProgress("loading");
            if (ElapsedUs(g_loadStarted) > 600u * 1000u * 1000u) {
                WriteDiag();
                Fail(kErrLoadTimeout, "the race did not finish loading in 10 minutes");
            }
            return; // keep running frames (neutral pads) until the race is in
        case kParked:
            if (LoadPending()) {
                SetState(kIdle);
                continue;
            }
            if (AbortPending()) {
                g_seenAbort = Mail(kAbortSeq);
                SetState(kIdle);
                continue;
            }
            if (GoPending()) {
                g_seenGo = Mail(kGoSeq);
                g_frame = 0;
                g_shared[kFrame] = 0;
                g_due = SteadyClock::now();
                g_drawPass = 0;
                ResetPerfWindow(g_due);
                SetState(kRacing);
                continue;
            }
            ParkUntilMailbox();
            continue;
        case kRacing: {
            if (LoadPending()) {
                SetState(kIdle);
                continue;
            }
            if (AbortPending()) {
                g_seenAbort = Mail(kAbortSeq);
                SetState(kIdle);
                continue;
            }
            const auto start = SteadyClock::now();
            const int padsToken = Eggy::Perf::Start(Eggy::Perf::kPads);
            const int got = eggy_pads(g_frame, reinterpret_cast<uint8_t*>(&g_shared[kPads]));
            Eggy::Perf::Stop(padsToken);
            const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - start);
            if (got < 0) {
                continue; // the wait was cancelled by abort or a new load
            }
            if (got == 0) {
                for (int i = 0; i < 4; ++i) g_shared[kPads + i] = 0;
            }
            // A long lockstep stall is a park: no retrace catch-up burst afterwards, and no race
            // frames owed for it either.
            if (waited > std::chrono::milliseconds(100)) {
                WebHost::ParkedNanosCounter().fetch_add(waited.count(), std::memory_order_relaxed);
                VI_HLE_EggyNoteHostPark(waited);
                g_due += waited;
            }
            g_padsLive = true;
            RebindIfMoved(ctx);
            return;
        }
        default:
            return;
        }
    }
}

void PostCalc(CpuContext* ctx, uint32_t calcUs) {
    if (g_state != kRacing || !g_padsLive) {
        if ((g_shared[kTicks] & 63) == 0) WriteDiag();
        return;
    }
    g_padsLive = false;
    const int frame = g_frame;
    g_logicUs[g_logicN++ % 128] = calcUs;
    // Away: "hands the port to the computer from the frame after `frame`".
    for (int port = 0; port < 4; ++port) {
        const int32_t awayFrame = Mail(kAway + port);
        if (awayFrame >= 0 && !g_awayApplied[port] && frame >= awayFrame) ApplyAway(ctx, port);
    }
    const int stage = Stage();
    const int raceFrames = RaceFrames();
    // The snapshots feed the page's touch controls and HUD: a quarter of the frames is plenty.
    // The hash is read by the lockstep on every 60th frame (lockstep.ts HASH_EVERY: the frames
    // where (frame + 1) % 60 == 0), logged every 120th and sent with the report.
    if ((frame & 3) == 0) {
        for (int port = 0; port < 4; ++port) SnapshotKart(port, stage, raceFrames);
        SnapshotHud(frame);
    }
    bool hashed = false;
    uint32_t hash = 0;
    if ((frame + 1) % 60 == 0 || (frame % 120) == 0) {
        hash = HashRace(frame);
        hashed = true;
        g_shared[kHash] = static_cast<int32_t>(hash);
        g_shared[kHashFrame] = frame;
    }
    g_shared[kStage] = stage;
    g_shared[kCountdown] = stage == 1 ? std::max(0, 240 - raceFrames) : -1;
    g_shared[kRaceTimer] = raceFrames;
    g_frame = frame + 1;
    g_shared[kFrame] = g_frame;
    g_due += FramePeriod();
    if ((frame % 30) == 29) PublishTiming();
    if ((frame % 120) == 0) {
        WriteDiag();
        const int32_t* k = &g_shared[kKarts + std::max(0, g_cfg[kCfgMyPort]) * kKartWords];
        float speed;
        std::memcpy(&speed, &k[2], 4);
        std::printf("[eggy] race frame %d stage %d raceFrames %d hash 0x%08x my kart speed %.2f place %d lap %d\n",
                    frame, stage, raceFrames, hash, static_cast<double>(speed), k[19], k[20]);
    }
    eggy_frame_done(frame);
    const int32_t finishAll = Mail(kFinishAll);
    const bool forced = finishAll >= 0 && frame >= finishAll;
    if (forced || stage >= 4) {
        if (!hashed) {
            hash = HashRace(frame);
            g_shared[kHash] = static_cast<int32_t>(hash);
            g_shared[kHashFrame] = frame;
        }
        for (int port = 0; port < 4; ++port) SnapshotKart(port, stage, raceFrames);
        SnapshotHud(frame);
        WriteReport(frame, hash, forced ? "finishAll" : "race over");
        // Finished by place while still on the track: the computer drives them home behind the
        // results, rather than karts rolling to a stop on neutral pads.
        if (forced && g_resultsSeconds > 0) {
            for (int port = 0; port < 4; ++port) {
                const int slot = g_slotOfPort[port];
                const uint32_t rp = slot >= 0 ? RacePlayer(slot) : 0;
                if (rp && !g_awayApplied[port] && (R32(rp + 0x38) & 2u) == 0) ApplyAway(ctx, port);
            }
        }
        SetState(kFinished);
        eggy_report_ready();
    }
}

} // namespace

// ---- Hooks ----

namespace {
// One calc of the game with Eggy's frame boundary around it.
void RunCalc(CpuContext* ctx) {
    PreCalc(ctx);
    const bool counted = PerfCounting();
    const auto start = SteadyClock::now();
    const int token = Eggy::Perf::Start(Eggy::Perf::kCalc);
    InvokeDirectCpu<0x8023AE60>(ctx);
    Eggy::Perf::Stop(token);
    const uint32_t calcUs = ElapsedUs(start);
    if (counted) ++g_perfWin.frames;
    PostCalc(ctx, calcUs);
}

// How many whole frames the race is behind the wall clock (0 on time or ahead). Past
// kMaxDebtFrames the schedule moves up and the rest is let go.
int FramesBehind(SteadyClock::time_point now) {
    if (g_state != kRacing || Mail(kPacingOff) != 0 || now < g_due) return 0;
    const auto period = FramePeriod();
    int64_t behind = Nanos(now - g_due) / Nanos(period);
    if (behind > kMaxDebtFrames) {
        g_due += period * (behind - kMaxDebtFrames);
        g_perfWin.dropped += static_cast<int32_t>(behind - kMaxDebtFrames);
        behind = kMaxDebtFrames;
    }
    g_perfWin.behindMax = std::max(g_perfWin.behindMax, static_cast<int32_t>(behind));
    return static_cast<int>(behind);
}
} // namespace

// RKSceneManager::calc (b EGG::SceneManager::calc): the frame boundary. A race that has fallen a
// whole frame behind the wall clock runs up to kMaxExtraCalcs more calcs here, before the next
// draw: the game keeps its real speed and the picture rate gives. Each extra calc is a full race
// frame (its own pads, snapshots, hash and onFrame), so the race is the same on every phone
// whatever each one draws.
extern "C" void Eggy_SceneCalc_80009984(CpuContext* ctx) {
    if (!Eggy::Enabled()) {
        InvokeDirectCpu<0x8023AE60>(ctx);
        return;
    }
    // calc leaves r3 (its `this`) clobbered; everything else it takes is preserved by the ABI.
    const uint32_t sceneManager = ctx->gpr[3];
    RunCalc(ctx);
    for (int extra = 0; extra < kMaxExtraCalcs && FramesBehind(SteadyClock::now()) >= 1; ++extra) {
        ++g_perfWin.extra;
        ctx->gpr[3] = sceneManager;
        RunCalc(ctx);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80009984, Eggy_SceneCalc_80009984, "Eggy_SceneCalc_80009984");

// RKSceneManager::draw (b EGG::SceneManager::draw): the start of a pass of the frame loop. Skipped
// on race frames the page asked not to see (renderSkip) and, during a race or its results, on all
// but one pass in drawEvery; timed otherwise. A skipped pass still gives the browser its turn
// (a drawn one does when it presents), so input and the lockstep's packets keep arriving.
extern "C" void Eggy_SceneDraw_80009988(CpuContext* ctx) {
    if (!Eggy::Enabled()) {
        InvokeDirectCpu<0x8023AEAC>(ctx);
        return;
    }
    const bool racing = g_state == kRacing;
    const bool counted = PerfCounting();
    if (counted) {
        const auto now = SteadyClock::now();
        const int64_t passUs = Nanos(now - g_perfWin.lastPass) / 1000;
        if (g_perfWin.passes > 0 && passUs > 20000) ++g_perfWin.late20;
        if (g_perfWin.passes > 0 && passUs > 34000) ++g_perfWin.late34;
        g_perfWin.lastPass = now;
        ++g_perfWin.passes;
        if (now - g_perfWin.start >= std::chrono::seconds(1)) PublishPerf(now);
    }
    const int every = std::clamp(Mail(kDrawEvery), 1, 4);
    const bool stepped = counted && every > 1 && (g_drawPass++ % every) != 0;
    g_skipThisDraw = stepped || (racing && Mail(kRenderSkip) != 0);
    if (g_skipThisDraw) {
        ++g_drawsSkipped;
        const Eggy::Perf::Scope yield(Eggy::Perf::kYield);
        WebHost::Yield();
        return;
    }
    if (counted) ++g_perfWin.drawn;
    const auto start = SteadyClock::now();
    {
        const Eggy::Perf::Scope draw(Eggy::Perf::kDraw);
        InvokeDirectCpu<0x8023AEAC>(ctx);
        // Test hook (the engine's eggy.html ?slowDraw=): a draw this much slower, to prove the
        // race keeps its pace on a phone that draws slowly.
        const int32_t slowUs = racing ? Mail(kSlowDrawUs) : 0;
        if (slowUs > 0) {
            const auto until = SteadyClock::now() + std::chrono::microseconds(std::min(slowUs, 200000));
            while (SteadyClock::now() < until) {
            }
        }
    }
    if (racing) {
        g_drawUs[g_drawN++ % 128] = ElapsedUs(start);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x80009988, Eggy_SceneDraw_80009988, "Eggy_SceneDraw_80009988");

// RaceConfig::initRace: the loading race takes the page's config (players, course, rules, seed),
// its local players are bound to their GameCube ports, and it gets one screen.
extern "C" void Eggy_InitRace_805302C4(CpuContext* ctx) {
    if (!Eggy::Enabled() || g_state != kLoading) {
        func_805302C4(ctx);
        return;
    }
    ++g_initRaceCalls;
    const uint32_t rc = ctx->gpr[3];
    WriteMenuScenario(rc);
    for (int k = 0; k < g_localCount; ++k) {
        const int at = kCfgPlayers + g_localSlot[k] * kCfgPlayerWords;
        BindLocal(ctx, k, g_cfg[at + 1], g_cfg[at + 4] == 1);
    }
    func_805302C4(ctx);
    WriteSeeds(rc + kRaceScenario); // initRace re-seeds VS races from the clock
    WriteSeeds(rc + kMenuScenario);
    PatchOneScreen(rc);
    g_initRaceSeen = true;
    LogScenario(rc, "initRace");
}
REGISTER_NATIVE_FUNCTION_AS(0x805302C4, Eggy_InitRace_805302C4, "Eggy_InitRace_805302C4");

// GameScene::setFramerate(bool is30): split screen drops the game to 30 fps; Eggy has one screen
// and always runs at 60.
extern "C" void Eggy_SetFramerate_8051AF84(CpuContext* ctx) {
    if (Eggy::Enabled()) {
        ++g_framerateCalls;
        ctx->gpr[4] = 0;
    }
    func_8051AF84(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8051AF84, Eggy_SetFramerate_8051AF84, "Eggy_SetFramerate_8051AF84");

// The race's HUD page (Pages::RaceHUD) takes its number of HUD slots from localPlayerCount, not
// from the screen count: OnInit makes one set of controls per local player and picks their layout
// by that count (CtrlRaceRankNum_<count>_<slot>, ...), and every per-frame function loops over the
// same count. With 2-4 humans on one screen it would draw the split-screen HUD of every local
// player over one full-screen camera. These are all the RaceHUD functions that read the count
// (RaceConfig race scenario +6, checked against every reader in the DOL/REL); while one of them
// runs, the count reads 1, so the page is the 1P HUD of HUD slot 0, hudPlayerIds[0] = this
// phone's player. The race itself (AI, objects, items) never runs inside them and keeps the real
// count.
namespace {
struct OneHudSlot {
    uint32_t addr = 0;
    uint8_t saved = 0;
    OneHudSlot() {
        const uint32_t rc = g_oneHudSlot ? Ptr(kRaceConfig) : 0;
        if (!rc) return;
        addr = rc + kRaceScenario + kLocalPlayerCount;
        saved = R8(addr);
        W8(addr, 1);
    }
    ~OneHudSlot() {
        if (addr) W8(addr, saved);
    }
    OneHudSlot(const OneHudSlot&) = delete;
    OneHudSlot& operator=(const OneHudSlot&) = delete;
};
} // namespace

extern "C" void Eggy_RaceHudOnInit_80856278(CpuContext* ctx) {
    const OneHudSlot one;
    func_80856278(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80856278, Eggy_RaceHudOnInit_80856278, "Eggy_RaceHudOnInit_80856278");

extern "C" void Eggy_RaceHudBeforeControlUpdate_80856CEC(CpuContext* ctx) {
    const OneHudSlot one;
    func_80856CEC(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80856CEC, Eggy_RaceHudBeforeControlUpdate_80856CEC,
                            "Eggy_RaceHudBeforeControlUpdate_80856CEC");

extern "C" void Eggy_RaceHudAfterControlUpdate_808574B8(CpuContext* ctx) {
    const OneHudSlot one;
    func_808574B8(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x808574B8, Eggy_RaceHudAfterControlUpdate_808574B8,
                            "Eggy_RaceHudAfterControlUpdate_808574B8");

// Called by red and blue shells (the targeted-player balloon).
extern "C" void Eggy_RaceHudOnItemObjTargeting_80858A50(CpuContext* ctx) {
    const OneHudSlot one;
    func_80858A50(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80858A50, Eggy_RaceHudOnItemObjTargeting_80858A50,
                            "Eggy_RaceHudOnItemObjTargeting_80858A50");

// Called by RaceCameraMgr::Update (the name and item balloons above the karts).
extern "C" void Eggy_RaceHudUpdateRaceBalloons_80858AE4(CpuContext* ctx) {
    const OneHudSlot one;
    func_80858AE4(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x80858AE4, Eggy_RaceHudUpdateRaceBalloons_80858AE4,
                            "Eggy_RaceHudUpdateRaceBalloons_80858AE4");

namespace Eggy {

void Initialize() noexcept {
    if (g_enabled >= 0) {
        return;
    }
    g_enabled = eggy_enabled() ? 1 : 0;
    if (!g_enabled) {
        return;
    }
    std::memset(g_shared, 0, sizeof g_shared);
    g_shared[kMagic] = 0x59474745; // 'EGGY'
    g_shared[kVersion] = 2;
    g_shared[kHashFrame] = -1;
    g_shared[kStage] = -1;
    g_shared[kCountdown] = -1;
    g_shared[kMusic] = 1;
    g_shared[kSfx] = 1;
    PutF32(kPace, 1.0f);
    g_shared[kFinishAll] = -1;
    for (int p = 0; p < 4; ++p) g_shared[kAway + p] = -1;
    g_shared[kDrawEvery] = 1;
    g_shared[kHud] = -1;
    g_state = kOff;
    std::printf("[eggy] Eggy mode: shared block at 0x%08x (%d bytes)\n",
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_shared)), static_cast<int>(sizeof g_shared));
    eggy_attach(g_shared, static_cast<int>(sizeof g_shared));
    SetState(kBooting);
}

bool Enabled() noexcept {
    if (g_enabled < 0) {
        Initialize();
    }
    return g_enabled == 1;
}

bool SkipEndRender() noexcept { return g_enabled == 1 && g_skipThisDraw; }

namespace Perf {

int Start(Part part) noexcept {
    if (g_enabled != 1 || !PerfCounting() || g_perfDepth >= static_cast<int>(g_perfOpen.size())) {
        return -1;
    }
    g_perfOpen[g_perfDepth] = PerfOpen{part, SteadyClock::now(), 0};
    return g_perfDepth++;
}

void Stop(int token) noexcept {
    if (token < 0 || token >= g_perfDepth) {
        return;
    }
    const auto now = SteadyClock::now();
    // Anything opened after this token and never stopped (a guest stack switched away inside it)
    // is closed here too, each part with its own time.
    while (g_perfDepth > token) {
        const PerfOpen open = g_perfOpen[--g_perfDepth];
        const int64_t total = Nanos(now - open.start);
        g_perfNs[open.part] += std::max<int64_t>(0, total - open.childNs);
        if (g_perfDepth > 0) g_perfOpen[g_perfDepth - 1].childNs += total;
    }
}

} // namespace Perf

bool OverridePads(PADStatus* statuses, uint32_t count) noexcept {
    if (!Enabled()) {
        return false;
    }
    ++g_shared[kPadCalls];
    const auto* pads = reinterpret_cast<const uint8_t*>(&g_shared[kPads]);
    for (uint32_t port = 0; port < count; ++port) {
        PADStatus s{};
        s.err = PAD_ERR_NONE;
        if (g_padsLive && port < 4) {
            const int32_t awayFrame = Mail(kAway + static_cast<int>(port));
            const bool away = awayFrame >= 0 && g_frame > awayFrame;
            if (!away) {
                const uint16_t buttons =
                    static_cast<uint16_t>(pads[port * 4] | (pads[port * 4 + 1] << 8)) & kRaceButtons;
                s.button = buttons;
                s.stickX = static_cast<int8_t>(pads[port * 4 + 2]);
                s.stickY = static_cast<int8_t>(pads[port * 4 + 3]);
                s.triggerL = (buttons & kPadL) ? 255 : 0;
                s.triggerR = (buttons & kPadR) ? 255 : 0;
            }
        }
        statuses[port] = s;
    }
    return true;
}

} // namespace Eggy
