// XInput HLE core: the game-facing entry points over a host pad table.

#include "hle/xinput/xinput.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <atomic>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include "hle/kernel/kernel_internal.h"
#include "hle/xinput/input_filter.h"
#include "hle/xinput/pad_script.h"

// Render's Swap counter (d3d8.h); this weak stand-in is used only when the
// D3D HLE is not linked in.
__attribute__((weak)) uint64_t rotk_d3d8_frame_count() { return rotk_vblank_count(); }
// The runtime's lifted body of a guest function (rotk_runtime.h); this weak
// stand-in (none) is used only when the runtime is not linked in (kernel tests).
__attribute__((weak)) HostFn rotk_original(uint32_t) { return nullptr; }

namespace rotkk::xinput {

namespace {

// XDEVICE_TYPE_GAMEPAD (XPP_DEVICE_TYPE) of this XBE: the game input init (0xC00F0) passes
// &0x147834 to XGetDevices / XInputOpen / XGetDeviceChanges (Ghidra 2026-11-02).
constexpr uint32_t kGamepadType = 0x00147834;
constexpr uint32_t kHandleBase = 0x58490000;   // 'XI' | port + 1
constexpr uint32_t kErrorSuccess = 0, kErrorIoPending = 997, kErrorNotConnected = 1167;

std::mutex g_mu;
RotkPadState g_pads[4];
uint8_t g_hidden[4];  // analog buttons the game reads as up until released (bit i = analog i)
uint32_t g_packet[4];
bool g_open[4];
uint16_t g_rumble[4][2];
uint32_t g_reported = 0;  // device mask last reported by XGetDeviceChanges
// Input latency: host time (steady clock, ns) of the device input behind the
// current state of a port (0 = none pending), until the game first reads it.
uint64_t g_stamp[4];
RotkInputLatency g_latency;
uint64_t g_latency_sum_us = 0;
uint32_t g_setstate_calls = 0, g_rumble_logged = 0;
// Between XInitDevices and the first XGetDeviceChanges the USB bus is still
// "enumerating": XGetDevices sees no pads, they arrive as insertions. The game
// marks a port live (0x51E0BC + port*0x38, FUN_000193E0) only on an insertion;
// pads opened from XGetDevices at boot (FUN_00018EA0) are not, and gameplay
// then shows "please reconnect the controller to port 1".
bool g_enumerating = false;

// Pad script (guarded by g_mu).
bool g_script = false;
std::vector<PadEvent> g_events;
size_t g_logged = 0;         // events already logged as applied
uint64_t g_script_frame = ~0ull;
uint64_t (*g_frame_source)() = &rotk_d3d8_frame_count;

void SetPadLocked(int port, const RotkPadState& s, uint64_t input_ns = 0) {
  RotkPadState& d = g_pads[port];
  if (std::memcmp(&d, &s, sizeof s) != 0) {
    g_packet[port]++;
    if (input_ns && !g_stamp[port]) g_stamp[port] = input_ns;
  }
  d = s;
}

uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The game read port p: the input behind its state reached the game now.
std::atomic<uint64_t> g_last_read_ns{0};

void NoteReadLocked(int p) {
  g_last_read_ns = SteadyNs();
  if (!g_stamp[p]) return;
  uint64_t now = SteadyNs();
  uint32_t us = now > g_stamp[p] ? uint32_t(std::min<uint64_t>((now - g_stamp[p]) / 1000, ~0u)) : 0;
  g_stamp[p] = 0;
  RotkInputLatency& L = g_latency;
  L.min_us = L.count ? std::min(L.min_us, us) : us;
  L.max_us = std::max(L.max_us, us);
  g_latency_sum_us += us;
  L.count++;
  L.avg_us = uint32_t(g_latency_sum_us / L.count);
  if (L.count % 600 == 0)
    Log("xinput: input -> game read latency over %u inputs: min %.1f / avg %.1f / max %.1f ms",
        L.count, L.min_us / 1000.0, L.avg_us / 1000.0, L.max_us / 1000.0);
}

// Port 0 from the script at the current frame.
void ScriptPoll() {
  static std::once_flag once;
  std::call_once(once, [] {
    const char* path = std::getenv("ROTK_PAD_SCRIPT");
    if (path && *path && !rotk_xinput_load_script(path)) std::_Exit(2);
  });
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_script) return;
  uint64_t f = g_frame_source();
  if (f == g_script_frame) return;
  g_script_frame = f;
  for (; g_logged < g_events.size() && g_events[g_logged].frame <= f; g_logged++)
    Log("xinput: frame %llu: %s", (unsigned long long)f, g_events[g_logged].text.c_str());
  SetPadLocked(0, PadStateAt(g_events, f));
}

uint32_t Connected() {
  uint32_t m = 0;
  for (int i = 0; i < 4; i++)
    if (g_pads[i].connected) m |= 1u << i;
  return m;
}

int PortOf(uint32_t h) {
  int p = int(h - kHandleBase) - 1;
  return p >= 0 && p < 4 ? p : -1;
}

// XINPUT_GAMEPAD: wButtons, bAnalogButtons[8], sThumbLX, LY, RX, RY (18 bytes).
void WriteGamepad(uint32_t va, const RotkPadState& s) {
  Wr16(va, s.buttons);
  for (int i = 0; i < 8; i++) Wr8(va + 2 + i, s.analog[i]);
  Wr16(va + 10, uint16_t(s.lx));
  Wr16(va + 12, uint16_t(s.ly));
  Wr16(va + 14, uint16_t(s.rx));
  Wr16(va + 16, uint16_t(s.ry));
}

// ROTK_XINPUT_TRACE=1 (or the kernel trace): every call that differs from the
// previous call of the same function (args, caller, result), with the frame.
bool XTrace() {
  static const bool on = [] {
    const char* v = std::getenv("ROTK_XINPUT_TRACE");
    return v && *v && *v != '0';
  }();
  return on || g_trace;
}

const char* const kNames[] = {"XInitDevices",          "XGetDevices",   "XGetDeviceChanges",
                              "XInputOpen",            "XInputClose",   "XInputGetCapabilities",
                              "XInputGetState",        "XInputSetState"};

// ROTK_CALL_AT=<entry>[,<entry>...] (dev/test hook, numbers hex except frames),
// run in order at the first XInputGetState of that frame or later, on the game
// thread that polls the pads, between the game's own calls:
//   <frame>:<va>[/<arg>...][@<ecx>]  call guest function va (stdcall/cdecl
//                                    args; @ecx for thiscall); the context
//                                    (registers, esp) is restored after
//   <frame>:copy:<dst>:<src>:<len>   copy guest memory
//   <frame>:poke:<va>:<dword>        write a dword
// E.g. the game's in-game save point (tail of FUN_00054DB0 at 0x4DD30, not a
// function entry): select slot 0, commit the state block 0x527B78 into slot
// 0 (+0x30, valid flag), open the save menu, then A in the pad script saves:
//   7000:4C140/0,7000:copy:529C10:527B78:2064,7000:poke:529BE0:1,7000:39A30/1E8058@1E7F9C
struct CallAt {
  enum Kind { kCall, kCopy, kPoke } kind;
  uint64_t frame;
  uint32_t va, a, b;  // call: va, ecx (b != 0: set) / copy: dst, src, len / poke: va, value
  std::vector<uint32_t> args;
  bool done;
};
std::vector<CallAt>& CallsAt() {
  static std::vector<CallAt> calls = [] {
    std::vector<CallAt> v;
    const char* e = std::getenv("ROTK_CALL_AT");
    for (const char* p = e; p && *p;) {
      char* end;
      CallAt k{CallAt::kCall, std::strtoull(p, &end, 10), 0, 0, 0, {}, false};
      if (*end != ':') Fatal("ROTK_CALL_AT: expected <frame>:... at \"%s\"", p);
      end++;
      if (!std::strncmp(end, "copy:", 5) || !std::strncmp(end, "poke:", 5)) {
        k.kind = end[0] == 'c' ? CallAt::kCopy : CallAt::kPoke;
        k.va = uint32_t(std::strtoul(end + 5, &end, 16));
        if (*end == ':') k.a = uint32_t(std::strtoul(end + 1, &end, 16));
        if (k.kind == CallAt::kCopy && *end == ':') k.b = uint32_t(std::strtoul(end + 1, &end, 16));
      } else {
        k.va = uint32_t(std::strtoul(end, &end, 16));
        while (*end == '/') k.args.push_back(uint32_t(std::strtoul(end + 1, &end, 16)));
        if (*end == '@') {
          k.a = uint32_t(std::strtoul(end + 1, &end, 16));
          k.b = 1;
        }
      }
      if (*end && *end != ',') Fatal("ROTK_CALL_AT: bad entry in \"%s\"", e);
      v.push_back(std::move(k));
      p = *end ? end + 1 : end;
    }
    return v;
  }();
  return calls;
}

void RunCallsAt(Ctx& c) {
  uint64_t f = rotk_d3d8_frame_count();
  for (CallAt& k : CallsAt()) {
    if (k.done || f < k.frame) continue;
    k.done = true;
    if (k.kind == CallAt::kCopy) {
      std::memmove(GP(k.va), GP(k.a), k.b);
      Log("xinput: frame %llu: ROTK_CALL_AT copy %08X <- %08X (%X bytes)", (unsigned long long)f,
          k.va, k.a, k.b);
      continue;
    }
    if (k.kind == CallAt::kPoke) {
      Wr32(k.va, k.a);
      Log("xinput: frame %llu: ROTK_CALL_AT poke %08X = %08X", (unsigned long long)f, k.va, k.a);
      continue;
    }
    Log("xinput: frame %llu: ROTK_CALL_AT %08X (%zu args%s)", (unsigned long long)f, k.va,
        k.args.size(), k.b ? ", ecx set" : "");
    Ctx saved = c;
    if (k.b) c.ecx = k.a;
    uint32_t r = g_host.call_guest(c, k.va, k.args.data(), int(k.args.size()));
    c = saved;
    Log("xinput: ROTK_CALL_AT %08X returned %08X", k.va, r);
  }
}

// ROTK_DUMP_AT=<frame>:<va>:<len>[,...] (dev, hex va/len): writes guest memory
// [va, va+len) to $ROTK_LOG_DIR/dump_<va>_<frame>.bin at that frame (same
// place as ROTK_CALL_AT), e.g. to find game state such as the options.
void RunDumpsAt() {
  struct Dump {
    uint64_t frame;
    uint32_t va, len;
    bool done;
  };
  static std::vector<Dump> dumps = [] {
    std::vector<Dump> v;
    for (const char* p = std::getenv("ROTK_DUMP_AT"); p && *p;) {
      char* end;
      Dump d{std::strtoull(p, &end, 10), 0, 0, false};
      if (*end == ':') d.va = uint32_t(std::strtoul(end + 1, &end, 16));
      if (*end == ':') d.len = uint32_t(std::strtoul(end + 1, &end, 16));
      if (!d.len || (*end && *end != ',')) Fatal("ROTK_DUMP_AT: expected <frame>:<va>:<len> at \"%s\"", p);
      v.push_back(d);
      p = *end ? end + 1 : end;
    }
    return v;
  }();
  uint64_t f = rotk_d3d8_frame_count();
  for (Dump& d : dumps) {
    if (d.done || f < d.frame) continue;
    d.done = true;
    const char* dir = std::getenv("ROTK_LOG_DIR");
    char path[1024];
    std::snprintf(path, sizeof path, "%s/dump_%08X_%llu.bin", dir && *dir ? dir : ".", d.va,
                  (unsigned long long)f);
    if (FILE* out = std::fopen(path, "wb")) {
      std::fwrite(GP(d.va), 1, d.len, out);
      std::fclose(out);
      Log("xinput: frame %llu: dumped %08X+%X to %s", (unsigned long long)f, d.va, d.len, path);
    }
  }
}

// ROTK_LOG_CALLS=<va>[/<nargs>][,...] (dev, hex va): wraps up to 8 guest
// functions; each call with a new (caller, ecx, args, eax) is logged once
// (at most 500 lines per function), then the lifted body runs. E.g. which
// text ids the game asks for (FUN_0003BAC0, thiscall, 1 arg; eax -> UTF-16):
//   ROTK_LOG_CALLS=3BAC0/1
struct LogCall {
  uint32_t va = 0, nargs = 0;
  HostFn orig = nullptr;
  std::mutex mu;
  std::vector<std::string> seen;
};
LogCall g_log_calls[16];

template <int I>
void LogCallFn(Ctx& c) {
  LogCall& l = g_log_calls[I];
  uint32_t ret = Rd32(c.esp), ecx = c.ecx, a[8] = {};
  for (uint32_t i = 0; i < l.nargs && i < 8; i++) a[i] = Arg(c, int(i));
  l.orig(c);
  char line[256];
  int n = std::snprintf(line, sizeof line, "%08X(ecx %X", l.va, ecx);
  for (uint32_t i = 0; i < l.nargs && i < 8; i++)
    n += std::snprintf(line + n, sizeof line - n, ", %X", a[i]);
  std::snprintf(line + n, sizeof line - n, ") from %08X -> %X", ret, c.eax);
  std::lock_guard<std::mutex> g(l.mu);
  if (l.seen.size() >= 500) return;
  for (const std::string& s : l.seen)
    if (s == line) return;
  l.seen.push_back(line);
  Log("calls: frame %llu: %s", (unsigned long long)rotk_d3d8_frame_count(), line);
}
template <int... I>
constexpr std::array<HostFn, sizeof...(I)> LogCallTable(std::integer_sequence<int, I...>) {
  return {LogCallFn<I>...};
}
constexpr auto kLogCallFns = LogCallTable(std::make_integer_sequence<int, 16>{});

template <uint32_t (*F)(Ctx&), int N, int Id>
void Wrap(Ctx& c) {
  ScriptPoll();
  if (Id == 6 && !CallsAt().empty()) RunCallsAt(c);
  if (Id == 6 && std::getenv("ROTK_DUMP_AT")) RunDumpsAt();
  uint32_t ret = Rd32(c.esp), a[4] = {};
  for (int i = 0; i < N && i < 4; i++) a[i] = Arg(c, i);
  c.eax = F(c);
  c.esp += 4 + 4 * N;
  if (!XTrace()) return;
  // Per function, only calls that differ from its previous one.
  static std::mutex mu;
  static std::string last[8];
  char line[128];
  std::snprintf(line, sizeof line, "%s(%X, %X, %X, %X) from %08X -> %X", kNames[Id], a[0], a[1],
                a[2], a[3], ret, c.eax);
  std::lock_guard<std::mutex> l(mu);
  if (last[Id] == line) return;
  last[Id] = line;
  Log("xinput: frame %llu: %s", (unsigned long long)rotk_d3d8_frame_count(), line);
}

// VOID XInitDevices(DWORD NumDeviceTypes, PXDEVICE_PREALLOC_TYPE)
uint32_t InitDevices(Ctx&) {
  std::lock_guard<std::mutex> l(g_mu);
  g_reported = 0;
  g_enumerating = true;
  return 0;
}

// DWORD XGetDevices(PXPP_DEVICE_TYPE)
uint32_t GetDevices(Ctx& c) {
  std::lock_guard<std::mutex> l(g_mu);
  if (Arg(c, 0) != kGamepadType) return 0;  // memory units etc.: none
  if (g_enumerating) return g_reported;
  g_reported = Connected();
  return g_reported;
}

// BOOL XGetDeviceChanges(PXPP_DEVICE_TYPE, PDWORD Insertions, PDWORD Removals)
uint32_t GetDeviceChanges(Ctx& c) {
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t ins = 0, rem = 0;
  if (Arg(c, 0) == kGamepadType) {
    g_enumerating = false;
    uint32_t now = Connected();
    ins = now & ~g_reported;
    rem = g_reported & ~now;
    g_reported = now;
  }
  Wr32(Arg(c, 1), ins);
  Wr32(Arg(c, 2), rem);
  return ins || rem;
}

// HANDLE XInputOpen(PXPP_DEVICE_TYPE, DWORD Port, DWORD Slot, PXINPUT_POLLING_PARAMETERS)
uint32_t Open(Ctx& c) {
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t port = Arg(c, 1);
  if (Arg(c, 0) != kGamepadType || port >= 4 || !g_pads[port].connected) return 0;
  g_open[port] = true;
  return kHandleBase + port + 1;
}

// VOID XInputClose(HANDLE)
uint32_t Close(Ctx& c) {
  std::lock_guard<std::mutex> l(g_mu);
  if (int p = PortOf(Arg(c, 0)); p >= 0) g_open[p] = false;
  return 0;
}

// DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES):
// SubType @0, Reserved @2, In.Gamepad @4 (18 bytes), Out.Rumble @0x16.
uint32_t GetCapabilities(Ctx& c) {
  int p = PortOf(Arg(c, 0));
  uint32_t caps = Arg(c, 1);
  std::lock_guard<std::mutex> l(g_mu);
  if (p < 0 || !g_pads[p].connected) return kErrorNotConnected;
  std::memset(GP(caps), 0, 0x1A);
  Wr8(caps, 1);  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
  RotkPadState all;
  all.buttons = 0xFF;
  for (auto& a : all.analog) a = 0xFF;
  all.lx = all.ly = all.rx = all.ry = int16_t(0xFFFF);
  WriteGamepad(caps + 4, all);
  Wr16(caps + 0x16, 0xFFFF);
  Wr16(caps + 0x18, 0xFFFF);
  return kErrorSuccess;
}

// DWORD XInputGetState(HANDLE, PXINPUT_STATE {dwPacketNumber, Gamepad})
uint32_t GetState(Ctx& c) {
  int p = PortOf(Arg(c, 0));
  uint32_t st = Arg(c, 1);
  std::lock_guard<std::mutex> l(g_mu);
  if (p < 0 || !g_pads[p].connected) return kErrorNotConnected;
  Wr32(st, g_packet[p]);
  RotkPadState s = g_pads[p];
  for (int i = 0; i < 8; i++)  // rotk_xinput_hide_until_release
    if (g_hidden[p] & (1u << i)) {
      if (s.analog[i] > 30) s.analog[i] = 0;
      else g_hidden[p] &= ~(1u << i);
    }
  WriteGamepad(st + 4, s);
  NoteReadLocked(p);
  return kErrorSuccess;
}

// DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK): header {dwStatus @0,
// hEvent @4, ...}, Rumble {wLeftMotorSpeed @0x42, wRightMotorSpeed @0x44}.
// Completes at once: status = ERROR_SUCCESS and the event is signalled (the
// game waits on it with INFINITE before the next request).
uint32_t SetState(Ctx& c) {
  int p = PortOf(Arg(c, 0));
  uint32_t fb = Arg(c, 1);
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (p < 0 || !g_pads[p].connected) return kErrorNotConnected;
    uint16_t lm = Rd16(fb + 0x42), rm = Rd16(fb + 0x44);
    // Evidence that the game drives the motors (FUN_00018F20): the first
    // call and the first few distinct non-zero requests.
    if (g_setstate_calls++ == 0)
      Log("xinput: frame %llu: first XInputSetState port %d: %04X/%04X",
          (unsigned long long)rotk_d3d8_frame_count(), p, lm, rm);
    if ((lm || rm) && (lm != g_rumble[p][0] || rm != g_rumble[p][1]) && g_rumble_logged < 8) {
      g_rumble_logged++;
      Log("xinput: frame %llu: rumble port %d: left %04X right %04X (call %u)",
          (unsigned long long)rotk_d3d8_frame_count(), p, lm, rm, g_setstate_calls);
    }
    g_rumble[p][0] = lm;
    g_rumble[p][1] = rm;
  }
  Wr32(fb, kErrorSuccess);
  if (uint32_t ev = Rd32(fb + 4))
    if (auto o = GetHandle(ev))
      if (o->disp_va) SetEventVa(o->disp_va);
  return kErrorIoPending;
}

}  // namespace

namespace {

// The donor's (HP2) camera overrides - GameOptionDefaults 0x4A8E0 and the
// modern orbit camera 0x22320 - were removed in 5b: in ROTK those VAs are other
// code (never in title/overrides.allow, so they never ran), and ROTK's camera
// is scripted per scene (the right stick does not steer it).
const RotkOverride kOverrides[] = {
    {0x00147CF3, &Wrap<InitDevices, 2, 0>, "XInitDevices"},
    {0x00148ABB, &Wrap<GetDevices, 1, 1>, "XGetDevices"},
    {0x00148ADD, &Wrap<GetDeviceChanges, 3, 2>, "XGetDeviceChanges"},
    {0x001487B4, &Wrap<Open, 4, 3>, "XInputOpen"},
    {0x0014880A, &Wrap<Close, 1, 4>, "XInputClose"},
    {0x00148816, &Wrap<GetCapabilities, 2, 5>, "XInputGetCapabilities"},
    {0x001489EE, &Wrap<GetState, 2, 6>, "XInputGetState"},
    {0x00148A61, &Wrap<SetState, 2, 7>, "XInputSetState"},
};

}  // namespace

}  // namespace rotkk::xinput

using namespace rotkk::xinput;

const RotkOverride* rotk_xinput_overrides(size_t* count) {
  static std::vector<RotkOverride> all = [] {
    std::vector<RotkOverride> v(std::begin(kOverrides), std::end(kOverrides));
    const char* e = std::getenv("ROTK_LOG_CALLS");
    size_t n = 0;
    for (const char* p = e; p && *p && n < kLogCallFns.size();) {
      char* end;
      LogCall& l = g_log_calls[n];
      l.va = uint32_t(std::strtoul(p, &end, 16));
      if (*end == '/') l.nargs = uint32_t(std::strtoul(end + 1, &end, 10));
      if (*end && *end != ',') rotkk::Fatal("ROTK_LOG_CALLS: expected <va>[/<nargs>] at \"%s\"", p);
      l.orig = rotk_original(l.va);
      if (!l.orig) rotkk::Fatal("ROTK_LOG_CALLS: %08X is not a lifted function", l.va);
      v.push_back({l.va, kLogCallFns[n], "ROTK_LOG_CALLS"});
      rotkk::Log("xinput: ROTK_LOG_CALLS %08X, %u args", l.va, l.nargs);
      n++;
      p = *end ? end + 1 : end;
    }
    return v;
  }();
  *count = all.size();
  return all.data();
}

void rotk_xinput_set_pad(int port, const RotkPadState& s) { rotk_xinput_set_pad_at(port, s, 0); }

void rotk_xinput_set_pad_at(int port, const RotkPadState& s, uint64_t input_ns) {
  if (port < 0 || port >= 4) return;
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_script) SetPadLocked(port, s, input_ns);
}

uint64_t rotk_xinput_now_ns() { return SteadyNs(); }

uint64_t rotk_xinput_last_read_ns() { return g_last_read_ns; }

RotkInputLatency rotk_xinput_latency() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_latency;
}

bool rotk_xinput_load_script(const char* path) {
  std::vector<PadEvent> ev;
  if (path) {
    // A file ("@file" too), else inline compact events ("450:A 456:-").
    std::string name = path[0] == '@' ? path + 1 : path, text;
    if (std::ifstream f{name}) {
      std::stringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    } else if (path[0] != '@' && std::strchr(path, ':') && !std::strchr(path, '/')) {
      text = path;
      name = "(inline)";
    } else {
      rotkk::Warn("xinput: cannot read pad script %s", name.c_str());
      return false;
    }
    std::string err;
    if (!ParsePadScript(text, &ev, &err)) {
      rotkk::Warn("xinput: pad script %s: %s", name.c_str(), err.c_str());
      return false;
    }
    rotkk::Log("xinput: pad script %s: %zu events, last at frame %llu; real input off", name.c_str(),
              ev.size(), ev.empty() ? 0ull : (unsigned long long)ev.back().frame);
  }
  std::lock_guard<std::mutex> l(g_mu);
  g_script = path != nullptr;
  g_events = std::move(ev);
  g_logged = 0;
  g_script_frame = ~0ull;
  if (!g_script) SetPadLocked(0, RotkPadState{});
  return true;
}

bool rotk_xinput_script_active() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_script;
}

void rotk_xinput_set_frame_source(uint64_t (*frame)()) {
  std::lock_guard<std::mutex> l(g_mu);
  g_frame_source = frame ? frame : &rotk_d3d8_frame_count;
  g_script_frame = ~0ull;
}

void rotk_xinput_hide_until_release(int port, int analog) {
  std::lock_guard<std::mutex> l(g_mu);
  if (port >= 0 && port < 4 && analog >= 0 && analog < 8) g_hidden[port] |= uint8_t(1u << analog);
}

RotkPadState rotk_xinput_get_pad(int port) {
  std::lock_guard<std::mutex> l(g_mu);
  return port >= 0 && port < 4 ? g_pads[port] : RotkPadState{};
}

void rotk_xinput_get_rumble(int port, uint16_t* left, uint16_t* right) {
  std::lock_guard<std::mutex> l(g_mu);
  *left = port >= 0 && port < 4 ? g_rumble[port][0] : 0;
  *right = port >= 0 && port < 4 ? g_rumble[port][1] : 0;
}
