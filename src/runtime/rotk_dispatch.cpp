// Dispatch: every guest call reaches a lifted function through its slot
// (fp_<VA>, lifted with --overridable), an HLE override in that slot, a
// kernel export thunk or a host thunk. Plus the runtime services lifted code
// calls (lifted.h): fatal paths, CPUID, RDTSC, resume.

#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <utility>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/rotk_internal.h"
#include "runtime/platform.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
// table.cpp of the published lift (lifted with --overridable).
extern GuestFn* const kLiftedSlots[];
}  // namespace recomp

// Kernel HLE (src/hle/kernel/kernel_exports.h). Weak: the runtime links and
// runs without it, stopping at the first kernel call it cannot serve.
HostFn rotk_kernel_lookup(uint32_t ordinal) __attribute__((weak));
uint32_t rotk_kernel_data_va(uint32_t ordinal) __attribute__((weak));
uint64_t rotk_kernel_rdtsc() __attribute__((weak));

using namespace rotkrt;

void rotk_game_fixes_link();  // rotk_game_fixes.cpp

namespace {

// Slot of every lifted function, by va - code_lo.
std::vector<recomp::GuestFn*> g_slots;
uint32_t g_code_lo = 0, g_code_size = 0;

struct Pending {
  uint32_t va;
  HostFn fn;
  const char* name;
};
std::vector<Pending>& PendingOverrides() {
  static std::vector<Pending> v;
  return v;
}

// Kernel exports: one per ordinal.
struct KernelExport {
  HostFn fn = nullptr;
  const char* name = "?";
};
KernelExport g_kernel[512];

// Host thunks.
struct HostThunk {
  HostFn fn;
  const char* name;
};
std::vector<HostThunk> g_host_thunks;
std::mutex g_host_thunk_mu;

recomp::GuestFn* SlotOf(uint32_t va) {
  if (va - g_code_lo >= g_code_size) return nullptr;
  return g_slots[va - g_code_lo];
}

thread_local uint32_t t_kord = 0;  // ordinal of the kernel call in progress

void KernelCall(RotkCtx& c, uint32_t ord) {
  const KernelExport& k = g_kernel[ord];
  const bool trace = rotk_ktrace_enabled();
  const uint32_t ret = rotk_rd32(c.esp);
  if (trace)
    rotk_log("K> %-32s (%08X %08X %08X %08X) from %08X", k.name, rotk_arg(c, 0),
            rotk_arg(c, 1), rotk_arg(c, 2), rotk_arg(c, 3), ret);
  t_kord = ord;
  k.fn(c);
  if (trace) rotk_log("K< %-32s = %08X", k.name, c.eax);
}

// A kernel function export nobody implements: stop with its name.
void MissingKernelExport(RotkCtx& c) {
  rotk_log("rotk: kernel export #%u %s not implemented (called from %08X)", t_kord,
          rotk_kernel_name(t_kord), rotk_rd32(c.esp));
  rotk_exit(10, "unimplemented kernel export");
}

// ROTK_STOP_AT=<section>: every function of that section stops the run when
// called (phase-1 milestone: first call into D3D).
std::string g_stop_section;
void StopStub(RotkCtx& c) {
  uint32_t ret = rotk_rd32(c.esp);
  uint32_t callee = 0;
  if (rotk_rd8(ret - 5) == 0xE8) callee = ret + rotk_rd32(ret - 4);
  rotk_log("rotk: first call into %s: %08X from %08X (esp %08X)", g_stop_section.c_str(),
          callee, ret, c.esp);
  rotk_exit(0, ("reached " + g_stop_section).c_str());
}

// ROTK_STUB=va:eax:nargs[,...] (dev switch, for bisecting boot blockers):
// the guest function at va returns eax at once, popping nargs stack args.
struct Stub {
  uint32_t va, eax, nargs;
};
Stub g_stubs[16];
template <int I>
void StubFn(RotkCtx& c) {
  rotk_log("rotk: stub %08X -> %08X", g_stubs[I].va, g_stubs[I].eax);
  rotk_ret_stdcall(c, g_stubs[I].eax, int(g_stubs[I].nargs));
}
template <int... I>
constexpr std::array<HostFn, sizeof...(I)> StubTable(std::integer_sequence<int, I...>) {
  return {StubFn<I>...};
}
constexpr auto kStubFns = StubTable(std::make_integer_sequence<int, 16>{});

void ApplyStubs() {
  std::string spec = Env("ROTK_STUB");
  size_t n = 0;
  for (size_t pos = 0; pos < spec.size() && n < kStubFns.size();) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    Stub st{};
    if (std::sscanf(spec.substr(pos, end - pos).c_str(), "%x:%x:%u", &st.va, &st.eax,
                    &st.nargs) >= 2) {
      g_stubs[n] = st;
      if (!rotk_override(st.va, kStubFns[n])) rotk_fatal("ROTK_STUB: %08X is not a function", st.va);
      rotk_log("rotk: ROTK_STUB %08X -> eax %08X, %u args", st.va, st.eax, st.nargs);
      n++;
    }
    pos = end + 1;
  }
}

}  // namespace

void rotk_register_override_at_start(uint32_t va, HostFn fn, const char* name) {
  if (!g_slots.empty()) {
    if (!rotk_override(va, fn)) rotk_fatal("override %s: %08X is not a function", name, va);
    return;
  }
  PendingOverrides().push_back({va, fn, name});
}


// ---- donor gate (XboxPortKit, rule R-D1) ----------------------------------
// This runtime came from a donor port; its HLE registers overrides at the
// donor title's VAs. An override is applied only if its VA is listed in
// title/overrides.allow (one hex VA per line, '#' comments), which
// `portkit remap` fills with re-located XDK entries and the roles extend as
// they re-derive game addresses. ROTK_DONOR_GATE=0 turns the gate off.
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
namespace {
bool DonorGateAllows(uint32_t va) {
  static const std::set<uint32_t>* allow = [] {
    auto* s = new std::set<uint32_t>;
    const char* off = std::getenv("ROTK_DONOR_GATE");
    if (off && off[0] == '0') return static_cast<std::set<uint32_t>*>(nullptr);
    const char* path = std::getenv("ROTK_OVERRIDES_ALLOW");
    // ROTK_OVERRIDES_ALLOW=<file> replaces the list built in at compile time (title/overrides.allow)
    static const char kBuiltIn[] =
#include "overrides_allow.inc"
        ;
    std::ifstream file;
    std::istringstream built(kBuiltIn);
    if (path) file.open(path);
    std::istream& f = path ? static_cast<std::istream&>(file) : built;
    std::string line;
    while (std::getline(f, line)) {
      auto h = line.find('#');
      if (h != std::string::npos) line.resize(h);
      if (line.find_first_not_of(" \t") == std::string::npos) continue;
      s->insert(uint32_t(std::strtoul(line.c_str(), nullptr, 16)));
    }
    return s;
  }();
  return !allow || allow->count(va);
}
}  // namespace

bool rotk_override(uint32_t va, HostFn fn) {
  if (!DonorGateAllows(va)) {
    rotk_log("donor gate: override at %08X not confirmed for this title (title/overrides.allow); skipped", va);
    return true;
  }
  recomp::GuestFn* s = SlotOf(va);
  if (!s) return false;
  *s = fn;
  return true;
}

HostFn rotk_original(uint32_t va) {
  for (uint32_t i = 0; i < recomp::kLiftedFunctionCount; i++)
    if (recomp::kLiftedFunctions[i].va == va) return recomp::kLiftedFunctions[i].fn;
  return nullptr;
}

bool rotk_is_function(uint32_t va) { return SlotOf(va) != nullptr; }

uint32_t rotk_host_thunk(HostFn fn, const char* name) {
  std::lock_guard<std::mutex> lk(g_host_thunk_mu);
  uint32_t n = uint32_t(g_host_thunks.size());
  if ((n + 1) * 16 > rotk_layout::kHostThunkSize) rotk_fatal("host thunk region full");
  g_host_thunks.push_back({fn, name});
  return rotk_layout::kHostThunkBase + n * 16;
}

void rotk_dispatch(RotkCtx& c, uint32_t va) { recomp::Dispatch(c, va, 0); }

uint32_t rotk_call_guest(RotkCtx& c, uint32_t va, const uint32_t* args, int nargs) {
  const uint32_t esp0 = c.esp;
  for (int i = nargs - 1; i >= 0; i--) {
    c.esp -= 4;
    rotk_wr32(c.esp, args[i]);
  }
  c.esp -= 4;
  rotk_wr32(c.esp, 0xFEFEFEFE);  // return address: host
  recomp::Dispatch(c, va, 0);
  c.esp = esp0;
  return c.eax;
}

namespace rotkrt {

void InitDispatch() {
  recomp::g_guest_base = g_mem.base();
  g_code_lo = g_xbe.code_lo;
  g_code_size = g_xbe.code_hi - g_xbe.code_lo;
  g_slots.assign(g_code_size, nullptr);
  uint32_t n = 0;
  for (uint32_t i = 0; i < recomp::kLiftedFunctionCount; i++) {
    uint64_t va = recomp::kLiftedFunctions[i].va;
    if (va - g_code_lo < g_code_size) {
      g_slots[va - g_code_lo] = recomp::kLiftedSlots[i];
      n++;
    }
  }
  if (!SlotOf(g_xbe.entry))
    rotk_fatal("entry %08X is not in the lifted table (lift/LIFT_ID points at a lift of "
              "another XBE?)", g_xbe.entry);
  rotk_log("rotk: %u lifted functions in %08X..%08X", n, g_code_lo, g_xbe.code_hi);
  rotk_game_fixes_link();  // keeps rotk_game_fixes.cpp (its ROTK_OVERRIDEs) linked
  for (const Pending& p : PendingOverrides())
    if (!rotk_override(p.va, p.fn))
      rotk_fatal("override %s: %08X is not a lifted function", p.name, p.va);
  if (!PendingOverrides().empty())
    rotk_log("rotk: %zu overrides applied", PendingOverrides().size());
  PendingOverrides().clear();

  ApplyStubs();
  g_stop_section = Env("ROTK_STOP_AT");
  if (!g_stop_section.empty()) {
    uint32_t va, size, stubs = 0;
    if (!rotk_section(g_stop_section.c_str(), &va, &size))
      rotk_fatal("ROTK_STOP_AT=%s: no such section", g_stop_section.c_str());
    for (uint32_t a = va; a < va + size; a++)
      if (recomp::GuestFn* s = SlotOf(a)) {
        *s = StopStub;
        stubs++;
      }
    rotk_log("rotk: stop at the first call into %s (%u functions)", g_stop_section.c_str(),
            stubs);
  }
}

void BindKernelImports() {
  uint32_t funcs = 0, data = 0, missing = 0;
  std::string missing_names;
  for (uint32_t slot = g_xbe.kernel_thunk;; slot += 4) {
    uint32_t v = rotk_rd32(slot);
    if (!v) break;
    uint32_t ord = v & 0x7FFFFFFF;
    if (ord >= 512) rotk_fatal("kernel import ordinal %u out of range", ord);
    KernelExport& k = g_kernel[ord];
    k.name = rotk_kernel_name(ord);
    uint32_t dva = rotk_kernel_data_va ? rotk_kernel_data_va(ord) : 0;
    if (dva) {
      rotk_wr32(slot, dva);
      data++;
      continue;
    }
    HostFn fn = rotk_kernel_lookup ? rotk_kernel_lookup(ord) : nullptr;
    // Known data exports must come from the kernel HLE; without it they get
    // a zeroed block so the run can go on until a function is missing.
    static const uint32_t kDataOrdinals[] = {16,  22,  40,  64,  71,  102, 120, 154, 156,
                                             157, 162, 164, 240, 259, 321, 322, 323, 324,
                                             325, 326, 353, 354, 355, 356, 357};
    bool is_data = false;
    for (uint32_t d : kDataOrdinals) is_data |= d == ord;
    if (!fn && is_data) {
      uint32_t blk = rotk_hle_alloc(0x1000);
      rotk_wr32(slot, blk);
      rotk_log("rotk: warning: kernel data export #%u %s not provided: zeroed block %08X", ord,
              k.name, blk);
      data++;
      continue;
    }
    if (!fn) {
      fn = MissingKernelExport;
      missing++;
      missing_names += std::string(" ") + k.name;
    }
    k.fn = fn;
    rotk_wr32(slot, rotk_layout::kThunkBase + ord * rotk_layout::kThunkStride);
    funcs++;
  }
  rotk_log("rotk: kernel imports: %u functions, %u data, %u not implemented%s", funcs, data,
          missing, missing ? ":" : "");
  if (missing) rotk_log("  %s", missing_names.c_str());
}

namespace {
// Guest faults in lifted code go to the guest's SEH chain as
// STATUS_ACCESS_VIOLATION, raised through the kernel HLE's RtlRaiseException
// (it walks fs:[0] and calls the lifted handlers):
// - a handler that continues: the faulting instruction is retried when the
//   signal handler returns (it may have committed the page);
// - an __except block: a resume site throws GuestResume. A signal frame
//   cannot be unwound on this host, so the handler catches it and rewrites
//   the signal context so the faulting function "calls" RethrowFault; the
//   exception then leaves through ordinary frames to the function owning the
//   __except frame (lifted code is built with -fnon-call-exceptions and
//   frame records in leaf functions);
// - no handler: the kernel reports the unhandled exception and stops.
// Lifted code keeps guest registers in locals, so Ctx.esp is the last
// spilled value and the live stack may reach below it by up to one
// function's frame: at most 0xC00 bytes in this XBE (no _chkstk; largest
// `sub esp, imm` is 0xC00 at 0x1112BA). The exception is raised kFaultGap
// below it.
constexpr uint32_t kFaultGap = 0x1800;
constexpr int kMaxFaultDepth = 8;
thread_local int t_fault_depth = 0;
thread_local std::exception_ptr t_fault_exception;

[[noreturn]] void RethrowFault() {
  std::exception_ptr e = std::move(t_fault_exception);
  t_fault_exception = nullptr;
  std::rethrow_exception(e);
}

// Guest VA of the lifted function containing host pc (its symbol is
// recomp::gen::f_<VA>), 0 if pc is not in lifted code.
uint32_t LiftedFunctionAt(uintptr_t pc) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(pc), &info) || !info.dli_sname) return 0;
  const char* f = std::strstr(info.dli_sname, "6recomp3gen");
  if (!f || !(f = std::strstr(f, "f_"))) return 0;
  char hex[9] = {};
  std::memcpy(hex, f + 2, 8);  // f_XXXXXXXX, then the mangled signature
  return uint32_t(std::strtoul(hex, nullptr, 16));
}

// Raises the access violation on the current guest thread; true if a
// handler continued execution.
bool RaiseAccessViolation(RotkCtx& c, bool write, uint32_t addr, uint32_t fn_va) {
  HostFn raise = rotk_kernel_lookup ? rotk_kernel_lookup(302) : nullptr;  // RtlRaiseException
  if (!raise) return false;
  const uint32_t stack_limit = rotk_rd32(c.fs_base + 8);
  const uint32_t saved_esp = c.esp;
  if (c.esp < stack_limit + kFaultGap + 0x1000)
    rotk_fatal("guest stack exhausted raising an access violation at %08X (esp %08X, limit %08X)",
              addr, c.esp, stack_limit);
  // EXCEPTION_RECORD: code, flags, record, address, nparams, info[15].
  const uint32_t rec = (c.esp - kFaultGap - 0x50) & ~0xFu;
  std::memset(rotk_ptr(rec), 0, 0x50);
  rotk_wr32(rec + 0x00, 0xC0000005);
  rotk_wr32(rec + 0x0C, fn_va);  // the faulting function (instruction unknown)
  rotk_wr32(rec + 0x10, 2);
  rotk_wr32(rec + 0x14, write ? 1 : 0);
  rotk_wr32(rec + 0x18, addr);
  c.esp = rec - 8;
  rotk_wr32(c.esp + 4, rec);    // RtlRaiseException(record)
  rotk_wr32(c.esp, fn_va);      // "return address"
  raise(c);                    // returns only if a handler continued
  c.esp = saved_esp;
  return true;
}

void FaultHandler(int sig, siginfo_t* si, void* uc) {
  uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_mem.base());
  const bool guest = a - base < (1ull << 32);
  recomp::platform::FaultContext fc = recomp::platform::ReadFaultContext(uc);
  RotkCtx* c = rotk_current_ctx();
  const uint32_t fn = guest && c ? LiftedFunctionAt(fc.pc) : 0;
  if (fn && t_fault_depth < kMaxFaultDepth) {
    const uint32_t gaddr = uint32_t(a - base);
    if (rotk_ktrace_enabled())
      rotk_log("rotk: access violation (%s %08X) in %08X: raising", fc.write ? "write" : "read",
              gaddr, fn);
    ++t_fault_depth;
    bool cont;
    try {
      cont = RaiseAccessViolation(*c, fc.write, gaddr, fn);
    } catch (...) {
      --t_fault_depth;
      t_fault_exception = std::current_exception();
      if (recomp::platform::RedirectToCall(uc, RethrowFault)) return;
      rotk_fatal("cannot resume after a guest exception from a fault on this host");
    }
    --t_fault_depth;
    if (cont) return;  // retry the faulting instruction
  }
  if (guest)
    rotk_fatal("%s: guest memory fault (%s) at guest %08lX in %s%08X", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS",
              fc.write ? "write" : "read", static_cast<unsigned long>(a - base),
              fn ? "lifted f_" : "host code, near guest ", fn);
  rotk_fatal("%s: host fault at %p (runtime bug)", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS",
            si->si_addr);
}
}  // namespace

void InstallFaultHandler() {
  constexpr size_t kSize = 1u << 20;
  stack_t ss{};
  ss.ss_sp = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  ss.ss_size = kSize;
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = FaultHandler;
  // SA_NODEFER: a guest exception filter may fault again (nested raise).
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}

}  // namespace rotkrt

// ---- lifted.h services --------------------------------------------------
namespace recomp {

void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  if (GuestFn* s = SlotOf(target)) {
    (*s)(c);
    return;
  }
  if (target - rotk_layout::kThunkBase < rotk_layout::kThunkSize) {
    uint32_t ord = (target - rotk_layout::kThunkBase) / rotk_layout::kThunkStride;
    if (g_kernel[ord].fn && (target & (rotk_layout::kThunkStride - 1)) == 0) {
      KernelCall(c, ord);
      return;
    }
  }
  if (target - rotk_layout::kHostThunkBase < rotk_layout::kHostThunkSize) {
    uint32_t i = (target - rotk_layout::kHostThunkBase) / 16;
    if (i < g_host_thunks.size()) {
      g_host_thunks[i].fn(c);
      return;
    }
  }
  if (target - g_code_lo < g_code_size) {
    // Fixed format: lift/miss_roots.py collects these into manifest entries.
    rotk_log("rotk: dispatch-miss target=%08X from=%08X", target, va);
    rotk_fatal("indirect branch to unlifted address %08X from %08X (add it to "
              "lift/manifest.txt as an entry: lift/miss_roots.py <log>)", target, va);
  }
  rotk_fatal("indirect branch to %08X from %08X: not code", target, va);
}

void Resume(Ctx& c, GuestResume r) {
  for (;;) {
    try {
      Dispatch(c, uint32_t(r.target), uint32_t(r.from));
      return;
    } catch (const GuestResume& again) {
      if (again.ebp != r.ebp) throw;
      r = again;
    }
  }
}

void Unimplemented(Ctx&, uint32_t va, const char* what) {
  rotk_fatal("unimplemented instruction at %08X: %s", va, what);
}
void BadJump(Ctx&, uint32_t va, uint32_t target) {
  rotk_fatal("branch at %08X to %08X outside lifted code", va, target);
}
void Unreachable(Ctx&, uint32_t va) { rotk_fatal("returned from a no-return call at %08X", va); }
void Trap(Ctx&, uint32_t va) { rotk_fatal("breakpoint/trap instruction at %08X", va); }
void RaiseDivideError(Ctx&, uint32_t va) { rotk_fatal("integer divide error (#DE) at %08X", va); }
void Probe(Ctx& c, uint32_t va, const char* name) {
  rotk_log("probe: %s (%08X) ecx %08X args %08X %08X %08X %08X", name, va, c.ecx,
          rotk_arg(c, 0), rotk_arg(c, 1), rotk_arg(c, 2), rotk_arg(c, 3));
}

// The Xbox CPU: a 733 MHz Pentium III (Coppermine), family 6 model 8.
// MMX and SSE are claimed as on the console; SSE2 is not.
void Cpuid(Ctx& c) {
  switch (c.eax) {
    case 0:
      c.eax = 2;
      c.ebx = 0x756E6547;  // "GenuineIntel"
      c.edx = 0x49656E69;
      c.ecx = 0x6C65746E;
      break;
    case 1:
      c.eax = 0x00000683;
      c.ebx = 0;
      c.ecx = 0;
      c.edx = 0x0383F9FF;  // fpu .. cmov pat pse36 mmx fxsr sse
      break;
    default:
      c.eax = c.ebx = c.ecx = c.edx = 0;
      break;
  }
}

uint64_t Rdtsc(Ctx&) {
  if (rotk_kernel_rdtsc) return rotk_kernel_rdtsc();
  using namespace std::chrono;
  // 733.33 MHz, like the console.
  uint64_t ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  return ns * 11 / 15;
}

// The x64-guest overloads lifted.h declares are never referenced by 32-bit
// lifted code; nothing to define.

}  // namespace recomp
