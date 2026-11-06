// Binds the kernel HLE to the Lifter's runtime API (src/runtime/rotk_runtime.h).
// Without that header, or in the standalone unit-test build (ROTKK_NO_RUNTIME_GLUE:
// rotk_runtime is not linked there), it reports "no runtime".

#if __has_include("runtime/rotk_runtime.h") && !defined(ROTKK_NO_RUNTIME_GLUE)
#include "runtime/rotk_runtime.h"

#include "hle/kernel/kernel_internal.h"

namespace {

bool Commit(uint32_t va, uint32_t size, bool) { return rotk_commit(va, size); }
void Decommit(uint32_t va, uint32_t size) { rotk_decommit(va, size); }
uint32_t CallGuest(recomp::Ctx& c, uint32_t va, const uint32_t* args, int n) {
  return rotk_call_guest(c, va, args, n);
}
// The runtime builds its own stack/KPCR for the thread; the kernel then puts
// the thread on the stack/KPCR it laid out (rotkk::Prepare/Apply), which
// matches XAPI's TLS layout.
void Body(recomp::Ctx& c, void* user) {
  auto* fn = static_cast<std::function<void(recomp::Ctx&)>*>(user);
  (*fn)(c);
  delete fn;
}
void StartThread(std::function<void(recomp::Ctx&)> body) {
  auto* fn = new std::function<void(recomp::Ctx&)>(std::move(body));
  if (!rotk_spawn_thread(0x4000, Body, fn)) rotkk::Fatal("rotk_spawn_thread failed");
}

}  // namespace

bool rotkk_runtime_glue(RotkKernelHost* host, RotkKernelConfig* cfg) {
  host->guest_base = rotk_guest_base();
  host->commit = Commit;
  host->decommit = Decommit;
  host->call_guest = CallGuest;
  host->start_thread = StartThread;
  host->current_ctx = rotk_current_ctx;
  host->code_lo = rotk_xbe().code_lo;
  host->code_hi = rotk_xbe().code_hi;
  (void)cfg;  // defaults + ROTK_* environment
  return host->guest_base != nullptr;
}

// Subtitles On for the Ukrainian layer (owner 2026-11-06: Ukrainian by default, English
// speech). The game turns subtitles on by itself for a non-English language
// (FUN_0009BF50: language code != the default -> 0x16D5A4 = 1), but mods/uk replaces the
// English text bank, so the game still sees English and FUN_0009C930 (the session/profile
// defaults) leaves them Off. After those defaults, uk / uk_text set the flag the Options
// screen reads (FUN_0009BAE0); a loaded save still restores its own value (FUN_0009BB80).
namespace {
constexpr uint32_t kProfileDefaults = 0x9C930, kSubtitlesOn = 0x16D5A4;
void ProfileDefaults(RotkCtx& c) {
  rotk_original(kProfileDefaults)(c);
  if (rotkk::LanguageKey().rfind("uk", 0) == 0) {
    static bool logged = false;
    if (!logged) rotk_log("loc: subtitles on by default (language %s)", rotkk::LanguageKey().c_str());
    logged = true;
    rotk_wr8(kSubtitlesOn, 1);
  }
}
ROTK_OVERRIDE(kProfileDefaults, ProfileDefaults);
}  // namespace
#else
#include "hle/kernel/kernel_exports.h"
bool rotkk_runtime_glue(RotkKernelHost*, RotkKernelConfig*) { return false; }
#endif
