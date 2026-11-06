// ROTK.app's CFBundleExecutable (built by tools/install_app.sh; owner bug 2026-11-06).
// A bash script as the bundle's executable could not read the app's files when the app was
// opened from Finder under ~/Desktop (macOS privacy, TCC: the responsible process is the
// interpreter, no prompt, no log). This small Mach-O does what that script did, then execv()s
// the game in the same process, so macOS sees one signed binary of this bundle:
//   - player files in ROTK_PLAYER_ROOT (baked at install: ROTKPort/.cache/player), else
//     ~/Library/Application Support/ROTK/player: HOME=<root>/home (settings.toml, saves),
//     the log <root>/logs/rotk.log (stdout + stderr appended), ROTK_LOG_DIR=<root>/logs;
//   - ROTK_DATA = Contents/Resources (game/extracted inside), ROTK_MODS by language:
//     ROTK_LANG=uk -> Resources/mods/uk, en -> Resources/mods; the language comes from $ROTK_LANG,
//     else settings.toml [launcher] language (the launcher window's choice), else en (owner
//     2026-11-06: «мову обирають в лончері», English until the player picks Українська);
//   - ROTK_UI_LANG (en | uk, the same setting): the language of the installer's progress, its
//     dialogs and these alerts;
//   - a bundle without game data (the DMG build, tools/make_dmg.sh): the disc copy lives next to
//     the player folder (<root>/../data); on the first run the player picks the disc image
//     (ROTK_IMPORT_ISO skips the dialog) and Resources/import/import_disc.py extracts it there;
//   - the game is Contents/MacOS/rotk-bin ("rotk" would be this file on a case-insensitive disk);
//   - screen defaults unless ROTK_TEST_PROFILE=1: ROTK_WIDESCREEN=native and ROTK_RENDER_SCALE=native
//     (the display's aspect, Hor+, and resolution), ROTK_FPS60=1, ROTK_DRAW_DISTANCE =
//     settings [launcher] draw_distance or 1.5 (each only when not set).
// Every variable already in the environment wins (open --env, tests).
#include <CoreGraphics/CoreGraphics.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#ifndef ROTK_PLAYER_ROOT
#define ROTK_PLAYER_ROOT ""
#endif

static void mkdirs(const char* path) {
  char p[PATH_MAX];
  snprintf(p, sizeof p, "%s", path);
  for (char* s = p + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      mkdir(p, 0755);
      *s = '/';
    }
  mkdir(p, 0755);
}

static void setdef(const char* k, const char* v) { setenv(k, v, 0); }

// ROTK_DISPLAY_HZ=<current>,<max> for the launcher window (owner test 049f010: a ProMotion display
// set to 60 Hz must still offer 120 fps). Current: the highest refresh of the active displays' modes
// now; max: the highest of all their modes, or 120 on a MacBook Pro with ProMotion (built-in
// display) in case the mode list only holds the current rate.
static int promotion_model(void) {
  char m[64] = ""; size_t n = sizeof m;
  if (sysctlbyname("hw.model", m, &n, NULL, 0) != 0) return 0;
  static const char* ids[] = {"MacBookPro18,", "Mac14,5", "Mac14,6", "Mac14,9", "Mac14,10", "Mac15,3",
                              "Mac15,6", "Mac15,7", "Mac15,8", "Mac15,9", "Mac15,10", "Mac15,11",
                              "Mac16,1", "Mac16,5", "Mac16,6", "Mac16,7", "Mac16,8", NULL};
  for (int i = 0; ids[i]; i++) {
    size_t k = strlen(ids[i]);
    if (strncmp(m, ids[i], k) == 0 && (ids[i][k - 1] == ',' || m[k] == 0)) return 1;
  }
  return 0;
}
static void display_hz(void) {
  CGDirectDisplayID ids[16]; uint32_t n = 0; double cur = 0, mx = 0; int builtin = 0;
  if (CGGetActiveDisplayList(16, ids, &n) != kCGErrorSuccess) n = 0;
  for (uint32_t i = 0; i < n; i++) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(ids[i]);
    if (m) { double r = CGDisplayModeGetRefreshRate(m); if (r > cur) cur = r; CGDisplayModeRelease(m); }
    CFArrayRef a = CGDisplayCopyAllDisplayModes(ids[i], NULL);
    if (a) {
      for (CFIndex j = 0; j < CFArrayGetCount(a); j++) {
        double r = CGDisplayModeGetRefreshRate((CGDisplayModeRef)CFArrayGetValueAtIndex(a, j));
        if (r > mx) mx = r;
      }
      CFRelease(a);
    }
    if (CGDisplayIsBuiltin(ids[i])) builtin = 1;
  }
  if (cur < 1) cur = 60;
  if (mx < cur) mx = cur;
  if (mx < 120 && builtin && promotion_model()) mx = 120;
  char v[32]; snprintf(v, sizeof v, "%d,%d", (int)(cur + 0.5), (int)(mx + 0.5));
  setdef("ROTK_DISPLAY_HZ", v);
}

static char real_home[PATH_MAX] = "/tmp";

// settings.toml [launcher] <key> = "value" (or bare value); 0 if absent.
static int read_setting(const char* file, const char* key, char* out, size_t n) {
  FILE* f = fopen(file, "r");
  if (!f) return 0;
  char line[512];
  int in = 0, found = 0;
  size_t kl = strlen(key);
  while (fgets(line, sizeof line, f)) {
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '[') {
      in = strncmp(s, "[launcher]", 10) == 0;
      continue;
    }
    if (!in || strncmp(s, key, kl) || (s[kl] != ' ' && s[kl] != '=')) continue;
    char* v = strchr(s, '=');
    if (!v) continue;
    v++;
    while (*v == ' ' || *v == '"') v++;
    size_t i = 0;
    while (v[i] && v[i] != '"' && v[i] != ' ' && v[i] != '#' && v[i] != '\n' && i + 1 < n) out[i] = v[i], i++;
    out[i] = 0;
    found = 1;
  }
  fclose(f);
  return found;
}

static int ui_uk = 0;  // ROTK_UI_LANG=uk: installer and alerts in Ukrainian (default English)
#define TR(en, uk) (ui_uk ? (uk) : (en))

static void alert(const char* msg) {
  printf("launcher: alert: %s\n", msg);
  fflush(stdout);
  if (getenv("ROTK_NO_UI")) return;  // tests
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "/usr/bin/osascript -e 'display alert \"ROTK\" message \"%s\"' >/dev/null 2>&1", msg);
  system(cmd);
}

// First run of the DMG installer (coordinator 2026-11-06): the game code is built on this Mac.
// Copies this bundle to the target app (ROTK_INSTALL_APP, default ~/Desktop/ROTK.app), then
// Resources/build/build_game.sh lifts the player's default.xbe, compiles it with Apple Command
// Line Tools' clang (installing them with the system dialog when missing) and links
// <target>/Contents/MacOS/rotk-bin; a progress window (progress.js, JXA) shows its progress file.
static int build_game(const char* res, const char* data, const char* target) {
  char cmd[PATH_MAX * 4], prog[PATH_MAX], self[PATH_MAX];
  snprintf(self, sizeof self, "%s/..", res);  // .../Contents
  snprintf(cmd, sizeof cmd, "/bin/rm -rf '%s' && /usr/bin/ditto '%s/..' '%s' && /bin/rm -rf '%s/Contents/Resources/build' "
           "'%s/Contents/Resources/import'", target, self, target, target, target);
  if (system(cmd) != 0) {
    alert(TR("Could not create ROTK.app.", "Не вдалося створити ROTK.app."));
    return 1;
  }
  snprintf(prog, sizeof prog, "%s/build-progress.txt", data);
  unlink(prog);
  char work[PATH_MAX];  // lifted sources + objects: next to the data, never inside the app
  snprintf(work, sizeof work, "%s/build-work", data);
  setdef("ROTK_BUILD_WORK", work);
  if (!getenv("ROTK_NO_UI")) {
    snprintf(cmd, sizeof cmd, "/usr/bin/osascript -l JavaScript '%s/build/progress.js' '%s' >/dev/null 2>&1 &", res, prog);
    system(cmd);
  }
  printf("launcher: building the game code -> %s\n", target);
  fflush(stdout);
  snprintf(cmd, sizeof cmd, "/bin/bash '%s/build/build_game.sh' '%s/build' '%s/game/extracted' '%s/Contents/MacOS/rotk-bin' '%s'",
           res, res, data, target, prog);
  int rc = system(cmd);
  if (rc != 0) {
    char msg[512] = "", line[512];
    FILE* f = fopen(prog, "r");
    while (f && fgets(line, sizeof line, f)) snprintf(msg, sizeof msg, "%s", line);
    if (f) fclose(f);
    msg[strcspn(msg, "\n\"'")] = 0;
    char a[700];
    snprintf(a, sizeof a, TR("Build failed: %s", "Гру не зібрано: %s"), *msg ? msg : "?");
    alert(a);
    return 1;
  }
  snprintf(cmd, sizeof cmd, "/usr/bin/codesign --force --deep -s - '%s' >/dev/null 2>&1", target);
  system(cmd);
  return 0;
}

// First run of a bundle without game data: the player's own disc image -> <data>.
static int import_disc(const char* res, const char* data) {
  char iso[PATH_MAX] = "", cmd[PATH_MAX * 3];
  if (getenv("ROTK_IMPORT_ISO")) {
    snprintf(iso, sizeof iso, "%s", getenv("ROTK_IMPORT_ISO"));
  } else {
    snprintf(cmd, sizeof cmd, "/usr/bin/osascript -e 'POSIX path of (choose file with prompt "
                    "\"%s\")'", TR("Choose your The Return of the King disc image (Xbox, .iso)",
                    "Оберіть образ диска The Return of the King (Xbox, .iso)"));
    FILE* p = popen(cmd, "r");
    if (p) {
      if (!fgets(iso, sizeof iso, p)) iso[0] = 0;
      pclose(p);
    }
    iso[strcspn(iso, "\n")] = 0;
  }
  if (!*iso) return 1;
  mkdirs(data);
  printf("launcher: importing the disc %s -> %s\n", iso, data);
  fflush(stdout);
  snprintf(cmd, sizeof cmd, "/usr/bin/python3 -I '%s/import/import_disc.py' '%s' '%s'", res, iso, data);
  if (system(cmd) != 0) {
    alert(TR("Disc import failed (python3 is needed: xcode-select --install).",
              "Не вдалося імпортувати диск (потрібен python3: xcode-select --install)."));
    return 1;
  }
  char reg[64] = "", rf[PATH_MAX];
  snprintf(rf, sizeof rf, "%s/region.txt", data);
  FILE* f = fopen(rf, "r");
  if (f) {
    if (!fgets(reg, sizeof reg, f)) reg[0] = 0;
    fclose(f);
  }
  if (*reg && !strstr(reg, "US"))  // addendum g: the EU (PAL) disc is supported by known differences only
    alert(TR("EU disc (PAL): untested.", "Європейський диск (PAL): не перевірено."));
  return 0;
}

int main(int argc, char** argv) {
  char exe[PATH_MAX], real[PATH_MAX];
  uint32_t sz = sizeof exe;
  if (_NSGetExecutablePath(exe, &sz) != 0 || !realpath(exe, real)) return 70;
  char here[PATH_MAX];
  snprintf(here, sizeof here, "%s", dirname(real));  // .../ROTK.app/Contents/MacOS

  char root[PATH_MAX] = ROTK_PLAYER_ROOT;
  if (getenv("ROTK_PLAYER_ROOT")) snprintf(root, sizeof root, "%s", getenv("ROTK_PLAYER_ROOT"));
  struct stat st;
  char parent[PATH_MAX];
  snprintf(parent, sizeof parent, "%s", root);
  if (!*root || stat(dirname(parent), &st) != 0) {  // baked folder gone (another Mac): user library
    const char* h = getenv("HOME");
    snprintf(root, sizeof root, "%s/Library/Application Support/ROTK/player", h ? h : "/tmp");
  }
  // Under ~/Desktop macOS privacy (TCC) may refuse the baked folder: then the user library.
  char home[PATH_MAX], logs[PATH_MAX], log[PATH_MAX], sup[PATH_MAX];
  int fd = -1, denied = 0;
  for (int attempt = 0; attempt < 2 && fd < 0; attempt++) {
    if (attempt) {
      denied = errno;
      const char* h = getenv("HOME");
      snprintf(root, sizeof root, "%s/Library/Application Support/ROTK/player", h ? h : "/tmp");
    }
    snprintf(home, sizeof home, "%s/home", root);
    snprintf(logs, sizeof logs, "%s/logs", root);
    snprintf(sup, sizeof sup, "%s/Library/Application Support/ROTK", home);
    mkdirs(sup);
    mkdirs(logs);
    snprintf(log, sizeof log, "%s/rotk.log", logs);
    fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
  }
  if (getenv("HOME")) snprintf(real_home, sizeof real_home, "%s", getenv("HOME"));
  setenv("HOME", home, 1);
  setdef("ROTK_LOG_DIR", logs);
  if (fd >= 0) {
    time_t t = time(NULL);
    dprintf(fd, "=== %s", ctime(&t));
    dprintf(fd, "launcher: %s, player files %s\n", real, root);
    if (denied) dprintf(fd, "launcher: baked player folder refused (%s): user library used\n", strerror(denied));
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
  }

  {  // interface language of the installer and the alerts: the launcher's saved choice, else English
    char sf[PATH_MAX], ul[16] = "en";
    snprintf(sf, sizeof sf, "%s/settings.toml", sup);
    if (getenv("ROTK_UI_LANG") && *getenv("ROTK_UI_LANG")) snprintf(ul, sizeof ul, "%s", getenv("ROTK_UI_LANG"));
    else if (getenv("ROTK_LANG") && *getenv("ROTK_LANG")) snprintf(ul, sizeof ul, "%s", getenv("ROTK_LANG"));
    else read_setting(sf, "language", ul, sizeof ul);
    ui_uk = strcmp(ul, "uk") == 0;
    setenv("ROTK_UI_LANG", ui_uk ? "uk" : "en", 1);
  }
  char res[PATH_MAX], buf[PATH_MAX];
  snprintf(res, sizeof res, "%s/../Resources", here);
  snprintf(buf, sizeof buf, "%s/game/extracted/default.xbe", res);
  const char* env_data = getenv("ROTK_DATA");
  if (env_data && !*env_data) unsetenv("ROTK_DATA"), env_data = NULL;  // empty = not set
  if (!env_data && stat(buf, &st) != 0) {
    char data[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", root);
    snprintf(data, sizeof data, "%s/data", dirname(parent));
    snprintf(buf, sizeof buf, "%s/game/extracted/default.xbe", data);
    if (stat(buf, &st) != 0) {
      // DMG installer: python3 (import) and clang (build) come with Apple Command Line Tools
      char clt[PATH_MAX * 2];
      snprintf(clt, sizeof clt, "%s/build/clt.sh", res);
      if (stat(clt, &st) == 0) {
        snprintf(clt, sizeof clt, "/bin/bash '%s/build/clt.sh'", res);
        if (system(clt) != 0) {
          alert(TR("Command Line Tools are not installed (xcode-select --install).",
                    "Command Line Tools не встановлено (xcode-select --install)."));
          return 74;
        }
      }
      if (import_disc(res, data) != 0) return 72;
    }
    snprintf(res, sizeof res, "%s", data);
  }
  setdef("ROTK_DATA", res);
  char settings[PATH_MAX], lang[16] = "en";
  snprintf(settings, sizeof settings, "%s/settings.toml", sup);
  // The launcher window (tools/launcher/launcher.js): language, 60/120 fps, gamma, Play. Not in
  // test runs (ROTK_BACKGROUND / ROTK_NO_UI) unless ROTK_LAUNCHER_AUTO answers it; ROTK_LAUNCHER=0 skips.
  const char* lw = getenv("ROTK_LAUNCHER");
  int ui = !getenv("ROTK_NO_UI") && !getenv("ROTK_BACKGROUND");
  if ((!lw || strcmp(lw, "0") != 0) && (ui || getenv("ROTK_LAUNCHER_AUTO"))) {
    char js[PATH_MAX], art[PATH_MAX], cmd[PATH_MAX * 4], ans[64] = "";
    snprintf(js, sizeof js, "%s/../Resources/launcher/launcher.js", here);
    snprintf(art, sizeof art, "%s/../Resources/launcher/art.png", here);
    if (stat(art, &st) != 0) snprintf(art, sizeof art, "%s/launcher-art.png", res);
    display_hz();
    char me[16]; snprintf(me, sizeof me, "%d", (int)getpid());  // the game after execv: launcher.js
    setenv("ROTK_LAUNCHER_OWNER", me, 1);                         // yields activation to it
    printf("launcher: display %s Hz (now,max)\n", getenv("ROTK_DISPLAY_HZ"));
    if (stat(js, &st) == 0) {
      snprintf(cmd, sizeof cmd, "/usr/bin/osascript -l JavaScript '%s' '%s' '%s'", js, settings,
               stat(art, &st) == 0 ? art : "");
      FILE* p = popen(cmd, "r");
      if (p) {
        if (!fgets(ans, sizeof ans, p)) ans[0] = 0;
        pclose(p);
      }
      ans[strcspn(ans, "\n")] = 0;
      printf("launcher: window -> %s\n", *ans ? ans : "(no answer: play)");
      fflush(stdout);
      if (strcmp(ans, "quit") == 0) return 0;
    }
  }
  if (getenv("ROTK_LANG") && *getenv("ROTK_LANG")) snprintf(lang, sizeof lang, "%s", getenv("ROTK_LANG"));
  else read_setting(settings, "language", lang, sizeof lang);
  if (strcmp(lang, "uk") != 0) snprintf(lang, sizeof lang, "en");
  setenv("ROTK_UI_LANG", lang, 1);
  setenv("ROTK_LANG", lang, 1);
  snprintf(buf, sizeof buf, strcmp(lang, "uk") == 0 ? "%s/mods/uk" : "%s/mods", res);
  setdef("ROTK_MODS", buf);
  const char* tp = getenv("ROTK_TEST_PROFILE");
  if (!tp || strcmp(tp, "1") != 0) {
    setdef("ROTK_WIDESCREEN", "native");
    setdef("ROTK_RENDER_SCALE", "native");
    setdef("ROTK_FPS60", "1");
    char dd[16] = "1.5";  // draw distance (addendum d): settings.toml [launcher] draw_distance
    read_setting(settings, "draw_distance", dd, sizeof dd);
    setdef("ROTK_DRAW_DISTANCE", dd);
    char fps[16] = "60", gamma[16] = "original";  // launcher window: [launcher] fps, gamma
    read_setting(settings, "fps", fps, sizeof fps);
    read_setting(settings, "gamma", gamma, sizeof gamma);
    if (strcmp(fps, "120") == 0) setdef("ROTK_FPS", "120");
    // gamma: original | modern | warm (src/polish/postfx/postfx.mm); older numbers read as original
    if (strcmp(gamma, "modern") != 0 && strcmp(gamma, "warm") != 0) strcpy(gamma, "original");
    setdef("ROTK_GAMMA", gamma);
    // HD packs shipped in the app (DMG, owner 2026-11-06); the player's data dir has none
    const char* hd[][2] = {{"ROTK_HDUI", "hd_ui"}, {"ROTK_HDWORLD", "hd_world"}};
    for (int i = 0; i < 2; i++) {
      snprintf(buf, sizeof buf, "%s/../Resources/mods/%s/manifest.tsv", here, hd[i][1]);
      if (stat(buf, &st) == 0) { buf[strlen(buf) - strlen("/manifest.tsv")] = 0; setdef(hd[i][0], buf); }
    }
  }
  printf("launcher: language %s, mods %s, fps %s, gamma %s\n", lang, getenv("ROTK_MODS"),
         getenv("ROTK_FPS") ? getenv("ROTK_FPS") : "60", getenv("ROTK_GAMMA") ? getenv("ROTK_GAMMA") : "original");
  fflush(stdout);

  char game[PATH_MAX];
  snprintf(game, sizeof game, "%s/rotk-bin", here);  // not "rotk": same name as ROTK on APFS
  char kit[PATH_MAX];
  snprintf(kit, sizeof kit, "%s/../Resources/build/build_game.sh", here);
  if (stat(game, &st) != 0 && stat(kit, &st) == 0) {  // the DMG installer: no game code inside
    char target[PATH_MAX], r2[PATH_MAX];
    const char* h = real_home;
    if (getenv("ROTK_INSTALL_APP")) snprintf(target, sizeof target, "%s", getenv("ROTK_INSTALL_APP"));
    else snprintf(target, sizeof target, "%s/Desktop/ROTK.app", h);
    snprintf(game, sizeof game, "%s/Contents/MacOS/rotk-bin", target);
    snprintf(r2, sizeof r2, "%s/../Resources", here);
    if (stat(game, &st) != 0 && build_game(r2, res, target) != 0) return 73;
  }
  argv[0] = game;
  execv(game, argv);
  fprintf(stderr, "launcher: cannot start %s: %s\n", game, strerror(errno));
  return 71;
}
