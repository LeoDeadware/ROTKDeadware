// rotk: runs the lifted default.xbe.
//
//   rotk [path/to/default.xbe]
//
// Environment: ROTK_DATA (the data folder with game/extracted; default: the repo of a build tree
// binary, else the current folder),
// ROTK_LOG_DIR (runtime.log there), ROTK_KTRACE=0 (no kernel call trace),
// ROTK_STOP_AT=<section> (stop at the first call into it, e.g. D3D).

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "runtime/rotk_internal.h"
#if ROTK_HAVE_XINPUT
#include "hle/xinput/xinput.h"
#endif

// Kernel HLE: `rotk --help` text (kernel_exports.h); weak, as the kernel may be absent.
const char* rotk_help_text() __attribute__((weak));
// Polish (src/polish): reads its switches once, before the D3D8 HLE starts.
void rotk_polish_init() __attribute__((weak));

int main(int argc, char** argv) {
  using namespace rotkrt;
  if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    std::fputs(rotk_help_text ? rotk_help_text() : "usage: rotk [path/to/default.xbe]\n", stdout);
    return 0;
  }
  OpenLog();
  if (rotk_polish_init) rotk_polish_init();
  std::string xbe = argc > 1 ? argv[1] : Env("ROTK_XBE");
  if (Env("ROTK_DATA").empty()) {
    // No baked path (the binary ships in the DMG kit): a build tree's binary is
    // <repo>/out/src/loader/rotk and the repo holds game/extracted; otherwise the current folder.
    std::string d = ".";
    char exe[PATH_MAX], real[PATH_MAX];
    uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) == 0 && realpath(exe, real)) {
      std::string r = real;
      for (int up = 0; up < 4 && r.find('/') != std::string::npos; up++) r.erase(r.rfind('/'));
      if (access((r + "/game/extracted/default.xbe").c_str(), R_OK) == 0) d = r;
    }
    setenv("ROTK_DATA", d.c_str(), 0);
  }
  if (xbe.empty()) xbe = Env("ROTK_DATA") + "/game/extracted/default.xbe";
#if ROTK_HAVE_XINPUT
  // Queued now, applied when Boot builds the dispatch table.
  size_t n = 0;
  const RotkOverride* o = rotk_xinput_overrides(&n);
  for (size_t i = 0; i < n; i++) rotk_register_override_at_start(o[i].va, o[i].fn, o[i].name);
#endif
  Boot(xbe);
  RunMain();
}
