// Guest memory, HLE allocations, logging and stopping.

#include <algorithm>
#include <vector>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include "runtime/rotk_internal.h"
#include "runtime/log.h"

namespace rotkrt {

recomp::GuestMemory g_mem;
RotkXbeInfo g_xbe{};
std::vector<Section> g_sections;

namespace {
std::mutex g_log_mu;
FILE* g_log = nullptr;
std::mutex g_alloc_mu;
uint32_t g_hle_next = rotk_layout::kHleBase;
}  // namespace

std::string Env(const char* name, const char* def) {
  const char* v = std::getenv(name);
  return v ? v : def;
}

void OpenLog() {
  std::string dir = Env("ROTK_LOG_DIR");
  if (dir.empty()) return;
  g_log = std::fopen((dir + "/runtime.log").c_str(), "w");
}

}  // namespace rotkrt

using namespace rotkrt;


bool rotk_commit(uint32_t va, uint32_t size) {
  return g_mem.Commit(va, size, recomp::Prot::RW);
}
void rotk_decommit(uint32_t va, uint32_t size) { g_mem.Decommit(va, size); }
bool rotk_is_committed(uint32_t va) { return g_mem.IsCommitted(va); }

namespace {
// Bytes of the never-freed HLE region per calling host function, for the
// "in use" reports: a caller that allocates per event shows up as growth.
std::map<void*, uint64_t>& HleUsers() {
  static std::map<void*, uint64_t> m;
  return m;
}
void LogHleUsers(uint32_t used) {
  std::vector<std::pair<uint64_t, void*>> v;
  for (const auto& [fn, bytes] : HleUsers()) v.push_back({bytes, fn});
  std::sort(v.rbegin(), v.rend());
  rotk_log("rotk: HLE region %u KB of %u KB in use; largest callers:", used / 1024,
          rotk_layout::kHleSize / 1024);
  for (size_t i = 0; i < v.size() && i < 6; ++i) {
    Dl_info d{};
    dladdr(v[i].second, &d);
    rotk_log("rotk:   %8llu KB  %s", (unsigned long long)(v[i].first / 1024),
            d.dli_sname ? d.dli_sname : "?");
  }
}
}  // namespace

uint32_t rotk_hle_alloc(uint32_t size, uint32_t align) {
  std::lock_guard<std::mutex> lk(g_alloc_mu);
  if (align < 4) align = 4;
  uint32_t va = (g_hle_next + align - 1) & ~(align - 1);
  void* caller = __builtin_extract_return_addr(__builtin_return_address(0));
  // Attribute to the function that contains the call site.
  Dl_info d{};
  if (dladdr(caller, &d) && d.dli_saddr) caller = d.dli_saddr;
  HleUsers()[caller] += size;
  const uint32_t used0 = g_hle_next - rotk_layout::kHleBase;
  if (uint64_t(va) + size > uint64_t(rotk_layout::kHleBase) + rotk_layout::kHleSize) {
    LogHleUsers(used0);
    rotk_fatal("rotk_hle_alloc: region full (%u bytes asked)", size);
  }
  if (!g_mem.Commit(va, size ? size : 1, recomp::Prot::RW))
    rotk_fatal("rotk_hle_alloc: commit %08X failed", va);
  g_hle_next = va + size;
  const uint32_t used = g_hle_next - rotk_layout::kHleBase;
  if (used / (2u << 20) != used0 / (2u << 20) && used >= (4u << 20)) LogHleUsers(used);
  return va;
}

const RotkXbeInfo& rotk_xbe() { return g_xbe; }

bool rotk_section(const char* name, uint32_t* va, uint32_t* size) {
  for (const auto& s : g_sections)
    if (s.name == name) {
      *va = s.va;
      *size = s.size;
      return true;
    }
  return false;
}

// Kernel HLE crash reports (kernel_exports.h); weak so the runtime links
// without the kernel.
extern "C++" void rotk_crash_note(const char* line) __attribute__((weak));
extern "C++" const char* rotk_crash_report(const char* reason) __attribute__((weak));

namespace {
void LogLine(const char* prefix, const char* fmt, va_list ap) {
  char buf[2048];
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  if (rotk_crash_note) {  // the report's log tail, also without ROTK_LOG_DIR
    char line[2100];
    std::snprintf(line, sizeof line, "%s%s", prefix, buf);
    rotk_crash_note(line);
  }
  std::lock_guard<std::mutex> lk(g_log_mu);
  std::fprintf(stderr, "%s%s\n", prefix, buf);
  if (g_log) {
    std::fprintf(g_log, "%s%s\n", prefix, buf);
    std::fflush(g_log);
  }
}
}  // namespace

void rotk_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogLine("", fmt, ap);
  va_end(ap);
}

namespace {
void DumpGuest() {
  RotkCtx* c = rotk_current_ctx();
  if (!c) return;
  rotk_log("  eax=%08X ecx=%08X edx=%08X ebx=%08X", c->eax, c->ecx, c->edx, c->ebx);
  rotk_log("  esp=%08X ebp=%08X esi=%08X edi=%08X fs=%08X", c->esp, c->ebp, c->esi,
          c->edi, c->fs_base);
  // Return addresses on the stack that point into lifted code.
  std::string line = "  code addresses on the stack:";
  int shown = 0;
  for (uint32_t a = c->esp; shown < 16 && a < c->esp + 0x1000; a += 4) {
    if (!g_mem.IsCommitted(a)) break;
    uint32_t v = rotk_rd32(a);
    if (v >= g_xbe.code_lo && v < g_xbe.code_hi) {
      char b[16];
      std::snprintf(b, sizeof b, " %08X", v);
      line += b;
      shown++;
    }
  }
  rotk_log("%s", line.c_str());
}
}  // namespace

void rotk_fatal(const char* fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  rotk_log("FATAL: %s", msg);
  DumpGuest();
  void* frames[48];
  int n = backtrace(frames, 48);
  backtrace_symbols_fd(frames, n, 2);
  if (g_log) std::fflush(g_log);
  if (rotk_crash_report) rotk_crash_report(msg);  // ~/Library/Logs/rotk/rotk-crash-<time>.txt
  std::_Exit(3);
}

void rotk_exit(int code, const char* why) {
  rotk_log("rotk: exit %d: %s", code, why);
  if (g_log) std::fflush(g_log);
  std::fflush(stderr);
  std::_Exit(code);
}

const char* rotk_kernel_name(uint32_t ordinal) {
  static const std::map<uint32_t, const char*> kNames = {
#include "runtime/rotk_kernel_names.inc"
  };
  auto it = kNames.find(ordinal);
  return it == kNames.end() ? "?" : it->second;
}

bool rotk_ktrace_enabled() {
  static const bool on = Env("ROTK_KTRACE", "1") != "0";
  return on;
}

// x86recomp's FatalF (x87/SSE model, image loader) ends in our report.
namespace {
[[noreturn]] void RecompFatal(const char* msg) { rotk_fatal("%s", msg); }
const bool kFatalHook = (recomp::g_fatal_report = RecompFatal, true);
}  // namespace
