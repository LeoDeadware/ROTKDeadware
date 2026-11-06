// `rotk --help`: usage, controls and every ROTK_* option of all components
// (kernel_exports.h: rotk_help_text). Keep in sync with docs/CONTROLS.md and
// the components' headers (d3d8.h, dsound mixer.cpp, runtime).

#include "hle/kernel/kernel_exports.h"

const char* rotk_help_text() {
  return R"(ROTKPort - The Lord of the Rings: The Return of the King (Xbox, NTSC-US), native on macOS.

Usage: rotk [path/to/default.xbe]      (default: $ROTK_DATA/game/extracted/default.xbe)
       rotk --help

Controls (Xbox Series controller; keyboard = controller 1; full list: docs/CONTROLS.md)
  Pad          Keyboard         In play
  A            Space            speed attack (menus: select)
  Y            R                fierce attack
  B            E                physical attack: knock enemies back (menus: back)
  X            Q                parry: block blows and arrows
  RT           O                killing move on a downed enemy, stab from behind
  LT           U                ranged attack (needs ammo)
  RB           1                action: push ladders away, use objects
                                (the original Black button)
  LB           2                the original White button
  Left stick   W A S D          move, climb
  Right stick  I J K L          flick: attack (the camera is the game's own)
  Menu         Enter            pause (original Start)
  View         Esc, Backspace   original Back
  D-pad        arrow keys       menus
  Stick clicks F (left), H (right)
  The on-screen prompts show these Series buttons (ROTK_PROMPTS=xbox: the original glyphs).
  Fullscreen   Cmd+F or Alt+Enter                Quit  Cmd+Q

Profile and saves
  ~/Library/Application Support/ROTKPort/<profile>/   created on first run
    rotk.ini                 settings: language=en|uk|uk_text
    hdd/partition1/UDATA/   save games (back this up)
  Crash reports: ~/Library/Logs/rotk/rotk-crash-<time>.txt
  ~/Library/Application Support/ROTK/settings.toml   gamepad: stick invert,
                          sensitivity, curve, smoothing, dead zones, Nintendo
                          layout, rumble strength (written with comments on
                          first run)

Settings (environment; ROTK_* variables override rotk.ini)
  ROTK_DATA=dir          folder with game/extracted/default.xbe (default: found
                        next to the app)
  ROTK_PROFILE=name      profile folder (default "default")
  ROTK_LANG=key          en (English, US), uk (Ukrainian: _UK slot + mods/uk) or
                        uk_text (Ukrainian text, English speech: mods/uk/_UKT)
  ROTK_MODS=dir:dir      override folders searched before the disc (default
                        $ROTK_DATA/mods/uk when it exists)
  ROTK_RENDER_SCALE=N    render at N x the Xbox resolution (default 1)
  ROTK_WIDESCREEN=1      16:10 view
  ROTK_FULLSCREEN=1      start fullscreen
  ROTK_LOOK=name         authentic_hd (default), crt, authentic, modern
  ROTK_PROMPTS=series    button prompts as on the Xbox Series pad (default);
                        xbox: the original glyphs
  ROTK_AUDIO=0           no sound device
  ROTK_SKIP_MOVIES=1     skip the intro movies; ROTK_SKIP=0: no skipping of
                        movies (A / Menu) and cutscenes (hold 4 s)
  ROTK_VIDEO_FLAGS=n, ROTK_AV_PACK=n   Xbox video settings (default: 480i
                        standard AV pack)
  ROTK_ZCACHE=1          let the game use its Z:\ file cache

Tests and development (silent runs: ROTK_BACKGROUND=1 SDL_AUDIO_DRIVER=dummy
SDL_VIDEO_DRIVER=dummy; see tests/scripts/)
  ROTK_BACKGROUND=1      no window, never take focus, no real keyboard/gamepad
  ROTK_TEST_PROFILE=1    features that change timing or look stay off
  ROTK_LOG_DIR=dir       runtime.log, screenshots and dumps there
  ROTK_PAD_SCRIPT=file   scripted controller (src/hle/xinput/pad_script.h)
  ROTK_SHOT_FRAMES=a,b   PNG of these frames; ROTK_SHOT_EVERY=N every Nth;
                        ROTK_SHOT_DIR=dir
  ROTK_EXIT_AFTER=N      exit after N frames
  ROTK_NO_PACING=1       do not wait for the 60 Hz vblank
  ROTK_FPS60=1           run at 60 fps (the original is 30; the .app sets it)
  ROTK_TEST_CLOCK=frames experimental: kernel time and vblanks follow frames
  ROTK_AUDIO_WAV=file    also write the sound mix to a WAV
  ROTK_INPUT_<SECTION>_<KEY>=v   gamepad setting for this run (settings.toml keys)
  ROTK_PROMPTS_DUMP=dir  write the generated Series prompt glyphs as PNG
  ROTK_HDUI_DUMP=dir     dump the game's DXT textures (hash.png + index.tsv)
  ROTK_CALL_AT=..., ROTK_DUMP_AT=...   call guest code / dump memory at a frame
  ROTK_LOG_CALLS=<va>[/<nargs>],...   log distinct calls (args, caller, eax) of
                        up to 16 guest functions
  ROTK_FRAME_LOG=1       one log line per frame (fps)
  ROTK_KTRACE=0|1, ROTK_KTRACE_KERNEL=1, ROTK_XINPUT_TRACE=1, ROTK_D3D_TRACE=1,
  ROTK_DSLOG=1           traces
  ROTK_STUB, ROTK_STOP_AT, ROTK_CALL, ROTK_XBE   runtime bring-up switches
  ROTK_CRASH_DIR=dir     crash reports there instead of ~/Library/Logs/rotk
  Render and audio debugging: ROTK_DEV_*, ROTK_TEX_DUMP, ROTK_DUMP_MSL,
  ROTK_SHADER_CHECK, ROTK_AUDIO_SOLO, ROTK_DS_* (src/hle/d3d8/d3d8.h,
  src/hle/dsound/)
)";
}
