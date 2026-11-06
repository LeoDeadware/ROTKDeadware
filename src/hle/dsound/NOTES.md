# Audio on the disc and in the game (recon notes, Audio role)

Data: `game/extracted` of the main checkout (read-only). Checked with xxd/ffprobe
(ffmpeg 7, Homebrew) on 2026-10-30.

## How the game makes sound
- **Music**: the game decodes EA music itself (software, SSE code around 0x11F3B0
  on the sound thread 0x1176B0, 10 ms tick) and writes 5.1 PCM into **6 mono
  16-bit 44.1 kHz looping ring buffers** (0x117310: one buffer per speaker bin
  FL FR C LFE BL BR, ~50 ms each, fed via GetCurrentPosition). Menu music uses
  only FL/FR. The game's software mix is ~6 dB under the source file
  (frontend.mus first 10 s mean −38.8 dB; the rings −45.0 dB at unity), and the
  APU's default 2D headroom takes another 6 dB (see Gain staging).
- **SFX/voice**: 0x98 Xbox ADPCM mono voices + 0x1C PCM voices
  (0x117160, 48 kHz nominal; real rate via SetFrequency 0x117FE0, clamped
  188..191983 Hz). Panning/3D are done by the game through
  SetMixBinVolumes (no DSound 3D calls).
- **Movies**: XMV audio is a DSound stream per movie (DirectSoundCreateStream,
  2 packets, callback 0x14EB6F, Pause(SYNCHPLAYBACK) + SynchPlayback). Formats:
  ea_logo/hpengx PCM 16-bit 44.1 kHz stereo, warner Xbox ADPCM 44.1 kHz stereo.
  No wmav2 in these files.
- **DSP**: an effects image (0x18A468, 0x3360 bytes) is downloaded and an I3DL2
  listener set (reverb). Not emulated (no reverb send).

## Gain staging (this XBE, read from its DSOUND code and the game's sound module; 2026-11-02)
Replaces the donor (HP2, XDK 5028) text that stood here.
- **Voices**: the game creates 128 mono 16-bit 48 kHz voices (0x81610), each with 7 mixbins
  FL FR C LFE BL BR I3DL2, and calls **SetHeadroom(0)** on every one (movies included).
  Per path gain (mB) = volume − headroom + mixbin volume.
- **Panning**: 0x80FF0 maps 5 linear gains (0..0x3FFF) to FL, FR, BL, BR, C; LFE gets
  (FL+FR)/2 in mB. The game pans in 5.1 although XC_AUDIO_FLAGS is 0 (stereo): a sound to one
  side is e.g. FL 0 dB + BL −3 dB; the 44.1 kHz pairs (movie, gameplay music/ambience) sit on
  FL+BL / FR+BR at 0 / −3 dB.
- **Mixbin (submix) headroom**: DSOUND default 1 (6 dB) for bins 0..30, 0 for bin 31; settings
  ctor 0x13FFBC, APU init 0x1418EF → 0x141460 writes it to 0xFE820200 + 4×bin
  (NV1BA0_PIO_SET_SUBMIX_HEADROOM); HRTF headroom 0 (0x141429). The VP divides each bin's
  input by 2^headroom (xemu vp.c). The game never calls SetMixBinHeadroom. Emulated in
  `submix_gain` (`ROTK_AUDIO_SUBMIX_HEADROOM=n` overrides).
- **Stereo fold-down**: in stereo/mono DSOUND tells the GP image a downmix mode (+0xC = 2/1);
  the fold is done by the EP (encode DSP), not by the GP image. Disassembled (xemu DSP56300
  disassembler): EP stereo module (chunk 0x1ED0F8, 0xC9 words) L = L + .7071 C + .7071 Ls,
  R = R + .7071 C + .7071 Rs, LFE dropped, no normalisation; the 24-bit store saturates at
  full scale, so the hardware hard-clips what goes over. Its optional coefficient scale (config
  word 16 / 17 = -2 dB) is off in the default EP config block (0x1ED420). Mono module
  (0x1ECE08): .5 L + .7071 C + .5 R + .3536 Ls + .3536 Rs. = our ITU table; kept, no make-up.
  A normalised fold (÷2.41) was rejected: the movie (two 44.1 kHz ADPCM buffer voices on
  FL −6 + BL −316 mB / FR + BR, peak 0.92 and no overs with ITU) would lose 7.7 dB with it.
- **Envelopes (SetEG 0x13F49E)**: amplitude envelope (dwEG=1) → ENV0/ENVA/TAR_LFO_ENV
  (0x142CAA). The game sets it before Play (0x813B0 from voice setup 0x865C0) from a per-sound
  word w: attack (w>>20)×2, decay ((w>>16)&15)<<8, sustain ((w>>12)&15)<<4|15, release
  (w&0xFFF)×2 ≤ 0xFFF; default w = 0xF000. Stops are StopEx(DSBSTOPEX_ENVELOPE) from 0x80E40.
  Emulated after xemu: 1 unit = 512 samples, linear attack, exponential decay to sustain,
  release e^(−6.91t), then the voice stops. Multi (pitch/filter) envelope not emulated.
  `ROTK_AUDIO_NO_EG=1` turns them off (control).
- **Master**: DC blocker, then a look-ahead peak limiter (`limiter.h`: 1.3 ms look-ahead,
  threshold 0.98, 80 ms release, stereo-linked) instead of the old tanh knee. Stats per 10 s
  window and per run: peak and "limited" (samples over 0 dBFS) **before** the limiter,
  "after limiter" (must stay 0), seconds of gain reduction > 0.1 dB and the deepest gain.
- **Measured** (`src/hle/dsound/tests/levels.sh 9000`, intro movie + prologue, 300 s):

  | | movie peak / rms / over | prologue peak / rms / over (worst 10 s) | whole run over |
  |---|---|---|---|
  | before (headroom 0, no EG) | 1.84 / 0.53 / 68003 | 4.21 / 0.76 / 172697 | > 1M |
  | headroom 1 + EG | 0.92 / 0.26 / 0 | 2.28 / 0.36 / 15330 | 44060 |
  | same, EG off (control) | 0.92 / 0.26 / 0 | 2.40 / 0.38 / 16503 | 51066 |

  Upstream check (2026-11-02): mB conversion (0x80E00, amplitude), volumes, LFE/I3DL2 (dropped,
  can only add), envelopes and the voice pool match the game code; Pat01 gameplay has 0 overs
  (peak <= 0.75); the prologue's overs depend only on the unknown GP fold (FOLD=front peak 1.62,
  half 1.26). Fold settled: the EP downmix is ITU with no make-up and saturates, so the
  original clips these prologue peaks too; the limiter replaces that clip. The GP image's code
  is encrypted in the XBE (DSOUND 0x140ABC decrypts it, key "12345678"); fx0 = 1-in/8-out
  gain splitter (0.5), fx1 = I3DL2 reverb; the GP executive (0x146FB8) has no fold.
  Details: docs/handoff/audio.md.
  The over count stays at the source (the game's 5.1 mix folded to stereo); the limiter
  numbers are in the handoff (`docs/handoff/audio.md`).
- Not verified: hardware/xemu output level of the GP image (make-up gain, downmix
  coefficients), envelope timing on hardware (xemu's model), anything by ear.

## Measured behaviour (2026-10-31, silent paced runs, WAV + ROTK_DSLOG)
- **Music files**: menu = `frontend.mpf`/`frontend.mus`; level 1 = `Burrow.mpf`/`burrow.mus`
  (kernel path trace). Level music matches burrow.mus at a constant offset (corr 0.75-0.93, no drift).
  Banks in level 1: Harry_Generic/Harry_Vocal, Lev_Burrows, Step_Dirt (footsteps), Gnome, ...,
  _USA/Speech_* (voice-over).
- **Interactive map**: section jumps are driven by game frames. Each jump used to leave
  ~45-60 ms of silence (three 15-20 ms holes; paced every 18.93 s, unpaced every 1.85 s). Cause:
  the same cue-subsystem busy-flag race as the 0x115B30 crash (a section switch found the
  subsystem busy on the other thread and was silently skipped until a later frame). Fixed by
  engine_fixes.cpp: paced menu 127 s -> 0 holes in loud music (was 18 in the same span),
  0 click spikes (`tests/music_gaps.py`, clicks check). Level music (burrow) was already clean.
  **Correction 2026-10-31 (beta):** that check counted only exact-zero runs; the master DC blocker
  turns a hole into a decaying tail, so it missed a second, larger hole: ~62-71 ms at *every*
  jump (paced menu, 3 of 3). Cause: at a jump the engine (0x125F00 -> 0x125A70 -> 0x1249D0) first
  takes the map's "end" branch and stops the stream (0x1C860 called at 0x124D29), and resolves
  and queues the next section "immediate" (0x1CD40 -> QueueSegment(s, -1, ...)) only on a later
  update, 5 clock ticks (50 ms) later; the decoder runs dry in between (the source data has no
  silence there: frontend.mus decoded with ffmpeg). Fix (engine_fixes.cpp "Section jumps"): that
  stop is deferred to the next section resolve / any cue entry / 100 ms. Now 0-19 ms
  (`tests/music_jump_gaps.sh`: 1 ms RMS windows under 10 % of the music level around each
  logged jump; control ROTK_DS_NO_JUMP_FIX=1). The rest is the new section's start-up: the
  reader I/O + header parse at the next game frame's pump (15 ms after the queue) + decoder tick.
  Running the pump right after the queue or on the sound tick did not help (I/O not done yet).
  Not reached: the 5 ms target. A real fix would pre-queue the next section held (0x1C700
  releases it); not tried.
- **Options**: Music Volume default **50** (= the -6 dB vs the file), Sound Effects 100. SFX slider
  -> SetVolume ramps on effect voices to -10000; music slider -> the game's ring mix. Both at 0:
  output exactly silent.
- **Pause**: music rings go silent; one-shots are frozen at 188 Hz (DSBFREQUENCY_MIN via
  0x117FE0's clamp) and keep sounding as sub-200 Hz content (~0.05 rms) — what the APU does too.
  No ducking. Master output has a ~10 Hz DC blocker (AC-coupled like the console).
- **Reverb**: SetI3DL2Listener is never called (menu, level 1); rings send -10000 to the FX bin.
  The downloaded DSP image is not emulated; not worth an approximation until a level sets one.
- **Movies / A/V**: XMV shows video frames by the audio clock (packet completions, callback
  0x14EB6F). The mixer used to be paced by the SDL device's pull, and SDL's dummy driver pulls
  ~5.8% slow, so packets completed late and video lagged (hpengx 77.0 s for 72.96 s; found by
  Render). Now the mixer runs on the wall clock and only pushes to the device: hpengx 73.0 s,
  warner 6.5 s (6.474), ea_logo 3.3 s (3.2).

## Crash after Continue -> Potter A (AV at 0x115B30) — fixed 2026-10-31
Repro: paced loads of a save (`tests/loadloop.sh`, profile from Kernel's save_roundtrip) under
CPU load (6 `yes`): 1 crash in 16. Fault: sound tick 0x115BA0 -> 0x115B30 read 0xA9BABCCB
(speech-bank bytes used as a pointer).
Cause: the speech/music cue subsystem (0x124000..0x127000) guards each public entry with a
non-atomic busy flag `if (DF18 == 0 && enter(-1))` (0x1240A0 sets 0x54DF18/0x54DF0C) and
silently does nothing when busy. On the 1-CPU Xbox no caller ever finds another thread
halfway inside; here one can. In the crashed run the main thread's "stop and free all music"
0x1C940 -> 0x126B10 found it busy, closed no stream (frontend.mus never closed), then freed
the music memory; the level loader thread streamed Speech_Burrows.sdt over the still-open
frontend stream (slot 0x80BDFCF0) and the tick read garbage. Clean runs close it at frame
632, the bank reaches that memory at ~frame 675.
Fix (engine_fixes.cpp): all 11 entries that take the flag (every writer of 0x54DF18:
0x125F00 0x125FE0 0x126290 0x126610 0x126710 0x126800 0x126B10 0x126C40 0x126D60 0x126E20
0x126F10; 0x124100 is only called from them) and the stream close 0x115F10 (which unpublished
and freed a slot without the lock while the tick reads it) run inside the game's recursive
sound critical section 0x54B8D4 — serialised as on one CPU. No I/O throttling.
DSound streams, same class: Release now waits for an in-flight completion, finishes its own
pending completions synchronously (no callbacks) and drops them; Flush/FlushEx deliver the
flushed completions before returning; stale completions are matched by object serial (VAs
are recycled). After Release/Flush nothing touches the game's packet memory.
Proof: 46/46 save loads under CPU load (all reached frame 1500), level/pause/options run and
all three movies (A/V within 0.05 s) unchanged.

## Rare AV at 0x115880 (stream segment header, write to 0x18) — closed 2026-10-31
Kernel saw it once (pick_uk.pad + UK overlay, frame 3875, game thread, pump
0x116FE0 -> service 0x115BA0 -> 0x115880): the service read a segment
header (`SCHl`) while the slot had no segment for it (+0x134 current is 0 and
the +0x11C list empty, or current is the list's last), so the next-segment
pointer was 0. Not reproduced in 24 runs of the same scenario (4 Kernel, 20
here with ROTK_DS_PROBE). Measured flow (menu music, probes): the music engine
queues segment N+1 (QueueSegment 0x115CF0, game thread) a few ms before the
pump reads N+1's header; consume callbacks 0x1157B0 run in the sound tick;
all list writers hold 0x54B8D4, except that QueueSegment issues the reader
request before it locks, and Open/queries/setters do not lock at all.
Fix (engine_fixes.cpp): every public slot entry runs locked, plus guards in
0x115880 / 0x115A60 that release a chunk with no segment (0x12F980, the
game's own path for unwanted chunks) and log
`stream slot ...: header chunk with no queued segment ... dropped`.
Stress: `tests/stream_stress.sh <build> <n>` (ROTK_DS_STREAM_STRESS) — a second
guest thread (created through XAPI CreateThread 0x111E05, which the file
library needs) opens / queues / queries / closes streams n times while the
tick and the pump service them; 500 plain + 300 with ROTK_DS_QUEUE_WIDEN_MS=20:
PASS, 0 crashes, 0 dropped chunks. The control run (ROTK_DS_NO_SLOT_FIX=1)
also passes, so the stress proves the locked paths are safe, not that it
reproduces the original crash.

## Formats
| files | container | audio | ffmpeg |
|---|---|---|---|
| `EAMusic/*.mus` | EA `SCHl` stream (PT header) | EA ADPCM R1, 22050 Hz stereo | decodes (`adpcm_ea_r1`) |
| `EAMusic/*.mpf` | `xDFP` = EA PathFinder interactive-music map (sections/branches into the .mus) | — | no |
| `EASound/*.sdt`, `*.SDT` | EA `BNKl` bank: u16 version 5, u16 count, u32 size, offsets → `PT` headers (tags 0x82 ch, 0x84 rate, 0x85 samples, 0x86/0x87 loop, 0x88 data offset, 0xA0 codec2 = 0x14 Xbox ADPCM) | Xbox ADPCM | no (vgmstream would) |
| `*.snd` (generic, sfx_harry, level sets) | u32 count (generic: 83), then per entry u32 size, 0, magic `32547698 78563412 32547698`, a full RIFF/WAVE | Xbox ADPCM 0x69 mono 22050 Hz, 64 samples / 36 bytes | per-entry RIFF yes |
| `*.xmv` | Xbox XMV | wmv2 video + PCM or Xbox ADPCM audio | yes (`adpcm_ima_xbox`) |

## Xbox ADPCM
IMA ADPCM, per channel a 4-byte header {int16 sample, u8 step index, u8 0} then
32 bytes; stereo interleaves 4 bytes per channel. 64 samples per block per
channel: header sample + 63 nibble samples (the 64th nibble unused), nibble
expansion `((2d+1)*step)>>3`. Sample-exact with ffmpeg (`tests/adpcm_test.sh`).

## Verification references
- `frontend.mus` vs the menu mix: envelope correlation 0.96-0.97 for 15 s at a
  constant offset (no speed drift); after ~16 s the interactive map branches.
- warner.xmv audio vs the movie mix: envelope correlation 0.91.

## AV in 0x1157B0 (consume walks off the segment list) — guarded 2026-10-31
Lifter's wanders (Great Hall frame 17880, Diagon Alley 43862) on bb5e726: first "chunk with no
queued segment" drops (0x115880/0x115A60 guards), then 0x1157B0 consumed n > all queued bytes
and read `[0+0x1C]`. StreamConsume clamps n to the bytes the list holds (drops the call when the
list is empty) and logs "consume ... clamped". Same class as 0x115880; the three list walkers on
the tick (0x115880, 0x115A60, 0x1157B0) are now guarded; 0x115780 does not touch the list.
Not proven: the condition did not recur in 4 fixed runs (guard never fired); root cause (reader
ahead of the queue) open.
