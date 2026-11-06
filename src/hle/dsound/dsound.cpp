// DirectSound HLE (owner: Audio): XDK 5028 DSOUND at API level.
//
// The statically linked DSOUND library (section DSOUND, 0x1444E0..) drives
// the MCPX APU (MMIO 0xFE800000), the AC97 codec (0xFEC00000) and APU
// memory directly. None of it runs here: every entry the game and XMV call
// is replaced (table kApi below, identities in FUNCTIONS.md), objects are
// small guest blocks whose state lives in rotkds::Voice, and mixer.cpp plays
// them. Every other DSOUND entry is guarded: a call logs once and runs the
// original (ROTK_DSOUND_STRICT=1: stops instead), so a missed entry point
// shows up by name instead of as a fault on APU registers.
//
// Calling conventions: the C API wrappers are stdcall; their first argument
// is the interface pointer we handed out. XMV calls streams through the
// XMediaObject vtable (AddRef, Release, GetInfo, GetStatus, Process,
// Discontinuity, Flush), so streams carry a real vtable of host thunks.
//
// Env: ROTK_DSLOG=1 logs every API call.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>

#include "hle/dsound/dsound_internal.h"
#include "hle/kernel/kernel_exports.h"
#include "runtime/rotk_runtime.h"

namespace rotkds {
State g_ds;

bool test_clock_frames() {
  static const bool on = [] {
    const char* v = getenv("ROTK_TEST_CLOCK");
    return v && std::string(v) == "frames";
  }();
  return on;
}
}  // namespace rotkds

using namespace rotkds;

namespace {

constexpr uint32_t S_OK = 0;
constexpr uint32_t E_FAIL = 0x80004005;
constexpr uint32_t DSERR_INVALIDCALL = 0x88780032;

constexpr uint32_t kOrdNtSetEvent = 225;

constexpr uint32_t kMagic = 0x53443248;  // "H2DS" at object +4
constexpr uint32_t kObjSize = 0x20;

bool env_on(const char* n) {
  const char* v = getenv(n);
  return v && atoi(v) != 0;
}
const bool g_trace = env_on("ROTK_DSLOG");
// Under ROTK_TEST_CLOCK=frames each line also carries the vblank and the
// guest return address of the API call (to diff two runs' call streams).
#define DSLOG(fmt, ...)                                                          \
  do {                                                                           \
    if (g_trace) {                                                               \
      if (rotkds::test_clock_frames()) {                                          \
        RotkCtx* lc = rotk_current_ctx();                                          \
        rotk_log("ds: v%llu ret %08x " fmt, (unsigned long long)rotk_vblank_count(), \
                lc ? rotk_rd32(lc->esp) : 0u, ##__VA_ARGS__);                     \
      } else {                                                                   \
        rotk_log("ds: " fmt, ##__VA_ARGS__);                                      \
      }                                                                          \
    }                                                                            \
  } while (0)

// ---- guest objects ------------------------------------------------------

uint32_t g_ds_obj = 0;  // the IDirectSound singleton
int g_ds_refs = 0;
uint32_t g_vt_ds = 0, g_vt_buffer = 0, g_vt_stream = 0;
std::vector<uint32_t> g_free_objs;  // released object blocks (guarded by mu)

template <int I>
void VtTrap(RotkCtx& c) {
  rotk_fatal("dsound: unexpected call through an HLE object's vtable, slot %d (+0x%x), "
            "this=0x%08x ret=0x%08x", I, I * 4, rotk_arg(c, 0), rotk_rd32(c.esp));
}
constexpr HostFn kVtTraps[16] = {VtTrap<0>,  VtTrap<1>,  VtTrap<2>,  VtTrap<3>,
                                 VtTrap<4>,  VtTrap<5>,  VtTrap<6>,  VtTrap<7>,
                                 VtTrap<8>,  VtTrap<9>,  VtTrap<10>, VtTrap<11>,
                                 VtTrap<12>, VtTrap<13>, VtTrap<14>, VtTrap<15>};

uint32_t make_vtable(const HostFn* fns, int n, const char* what) {
  uint32_t vt = rotk_hle_alloc(16 * 4);
  for (int i = 0; i < 16; ++i) {
    HostFn f = i < n && fns[i] ? fns[i] : kVtTraps[i];
    rotk_wr32(vt + 4 * i, rotk_host_thunk(f, what));
  }
  return vt;
}

uint32_t new_object(uint32_t vtable, uint32_t kind) {  // needs mu
  uint32_t va;
  if (!g_free_objs.empty()) {
    va = g_free_objs.back();
    g_free_objs.pop_back();
    std::memset(rotk_ptr(va), 0, kObjSize);
  } else {
    va = rotk_hle_alloc(kObjSize);
  }
  rotk_wr32(va, vtable);
  rotk_wr32(va + 4, kMagic);
  rotk_wr32(va + 8, kind);
  return va;
}

Voice* voice(uint32_t va) {  // needs mu
  auto it = g_ds.voices.find(va);
  return it == g_ds.voices.end() ? nullptr : it->second;
}

Format read_wfx(uint32_t wfx) {
  Format f;
  if (!wfx) return f;
  f.tag = rotk_rd16(wfx);
  f.channels = rotk_rd16(wfx + 2) ? rotk_rd16(wfx + 2) : 1;
  f.rate = rotk_rd32(wfx + 4) ? rotk_rd32(wfx + 4) : 48000;
  f.block_align = rotk_rd16(wfx + 12);
  f.bits = rotk_rd16(wfx + 14);
  if (f.tag == kFmtXboxAdpcm) f.block_align = uint16_t(kAdpcmBlockBytes * f.channels);
  if (f.tag == kFmtPcm && f.block_align == 0) f.block_align = uint16_t(f.channels * f.bits / 8);
  return f;
}

// DSMIXBINS {dwMixBinCount, DSMIXBINVOLUMEPAIR* {dwMixBin, lVolume}}.
// routing (SetMixBins / desc): the pairs become the voice's bins, in order;
// otherwise (SetMixBinVolumes) only the volumes of those bins change.
void apply_mixbins(Voice& v, uint32_t mb, bool routing) {
  if (!mb) return;
  uint32_t n = rotk_rd32(mb), pairs = rotk_rd32(mb + 4);
  if (!pairs || n > 64) return;
  if (routing) v.nbins = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t bin = rotk_rd32(pairs + 8 * i);
    int32_t vol = int32_t(rotk_rd32(pairs + 8 * i + 4));
    if (bin >= uint32_t(kMixBins)) continue;
    v.bin_vol[bin] = vol < -10000 ? -10000 : vol > 0 ? 0 : vol;
    if (routing && v.nbins < 8) v.bins[v.nbins++] = uint8_t(bin);
  }
}

// The library's defaults when a desc has no DSMIXBINS (0x144841, tables at
// 0x14D404 by channel count and 0x14D944 for 3D voices, all at 0 mB), and
// its default headroom (0x145942).
void init_mix_defaults(Voice& v) {
  static const uint8_t k2d[3][6] = {{0, 1}, {0, 1, 4, 5}, {0, 1, 2, 3, 4, 5}};
  static const uint8_t k3d[5] = {6, 8, 7, 9, 10};
  bool is3d = v.flags & 0x10;  // DSBCAPS_CTRL3D
  const uint8_t* b = is3d ? k3d : k2d[v.fmt.channels <= 2 ? 0 : v.fmt.channels <= 4 ? 1 : 2];
  v.nbins = is3d ? 5 : v.fmt.channels <= 2 ? 2 : v.fmt.channels <= 4 ? 4 : 6;
  for (int i = 0; i < v.nbins; ++i) v.bins[i] = b[i];
  for (int32_t& x : v.bin_vol) x = 0;
  v.headroom = (is3d || (v.flags & 0x182000)) ? 0 : 600;
}

// ---- completions (packets done) ------------------------------------------
// The mixer (host thread) queues finished packets; a guest worker thread
// and DirectSoundDoWork deliver them: status + size, then the stream's
// callback or the packet's completion event, as the XDK DPC would.

}  // namespace

void rotkds::deliver_completions(RotkCtx& c) {
  for (;;) {
    std::lock_guard<std::recursive_mutex> dl(g_ds.deliver_mu);
    Completion d;
    {
      std::lock_guard<std::mutex> lk(g_ds.mu);
      if (g_ds.done.empty()) return;
      d = g_ds.done.front();
      g_ds.done.pop_front();
      // Release completes its own pending entries; a stale one never writes.
      Voice* v = voice(d.voice_va);
      if (!v || v->serial != d.voice_serial) continue;
    }
    if (d.pkt.completed_size_va) rotk_wr32(d.pkt.completed_size_va, d.completed);
    if (d.pkt.status_va) rotk_wr32(d.pkt.status_va, d.status);
    if (d.callback) {
      uint32_t args[3] = {d.cb_context, d.pkt.context, d.status};
      rotk_call_guest(c, d.callback, args, 3);
    } else if (d.pkt.context) {
      uint32_t args[2] = {d.pkt.context, 0};
      rotk_call_guest(c, rotk_kernel_thunk_va(kOrdNtSetEvent), args, 2);
    }
  }
}

namespace {

void worker_body(RotkCtx& c, void*) {
  rotk_log("dsound: completion worker running (guest thread %u)", rotk_current_thread()->id);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    deliver_completions(c);
  }
}

extern const HostFn kStreamVtbl[7];

void ensure_started() {
  static std::once_flag once;
  std::call_once(once, [] {
    static const HostFn ds_fns[1] = {};
    g_vt_ds = make_vtable(ds_fns, 0, "dsound_vt_ds");
    g_vt_buffer = make_vtable(ds_fns, 0, "dsound_vt_buffer");
    g_vt_stream = make_vtable(kStreamVtbl, 7, "dsound_vt_stream");
    if (!test_clock_frames()) rotk_spawn_thread(0x10000, worker_body, nullptr);
    mixer_start();
  });
}

// ---- creation ------------------------------------------------------------

uint32_t create_buffer(uint32_t desc, uint32_t* out) {
  if (!desc || !out) return DSERR_INVALIDCALL;
  ensure_started();
  uint32_t flags = rotk_rd32(desc + 4), bytes = rotk_rd32(desc + 8);
  uint32_t wfx = rotk_rd32(desc + 12), mixbins = rotk_rd32(desc + 16);
  auto* v = new Voice;
  v->flags = flags;
  v->fmt = read_wfx(wfx);
  init_mix_defaults(*v);
  apply_mixbins(*v, mixbins, true);
  if (bytes) {  // the library allocates the data; SetBufferData may replace it
    v->data_va = rotk_hle_alloc(bytes, 32);
    v->data_bytes = bytes;
    rotk_log("dsound: buffer with %u own bytes at 0x%08x", bytes, v->data_va);
  }
  std::lock_guard<std::mutex> lk(g_ds.mu);
  v->va = new_object(g_vt_buffer, 1);
  v->serial = g_ds.next_serial++;
  g_ds.voices[v->va] = v;
  *out = v->va;
  DSLOG("CreateSoundBuffer flags=0x%x bytes=%u fmt=0x%x ch=%u rate=%u -> 0x%08x", flags, bytes,
        v->fmt.tag, v->fmt.channels, v->fmt.rate, v->va);
  return S_OK;
}

uint32_t create_stream(uint32_t desc, uint32_t* out) {
  if (!desc || !out) return DSERR_INVALIDCALL;
  ensure_started();
  auto* v = new Voice;
  v->stream = true;
  v->flags = rotk_rd32(desc);
  v->max_packets = rotk_rd32(desc + 4);
  v->fmt = read_wfx(rotk_rd32(desc + 8));
  v->callback = rotk_rd32(desc + 12);
  v->cb_context = rotk_rd32(desc + 16);
  init_mix_defaults(*v);
  apply_mixbins(*v, rotk_rd32(desc + 20), true);
  std::lock_guard<std::mutex> lk(g_ds.mu);
  v->va = new_object(g_vt_stream, 2);
  v->serial = g_ds.next_serial++;
  g_ds.voices[v->va] = v;
  *out = v->va;
  rotk_log("dsound: CreateSoundStream fmt=0x%x ch=%u rate=%u packets=%u cb=0x%08x -> 0x%08x",
          v->fmt.tag, v->fmt.channels, v->fmt.rate, v->max_packets, v->callback, v->va);
  return S_OK;
}

uint32_t release_voice(uint32_t va) {
  // Waits for a completion of this stream being delivered right now.
  std::lock_guard<std::recursive_mutex> dl(g_ds.deliver_mu);
  std::lock_guard<std::mutex> lk(g_ds.mu);
  Voice* v = voice(va);
  if (!v) return 0;
  if (--v->refs > 0) return uint32_t(v->refs);
  // A destroyed stream's packets end here, synchronously and without
  // callbacks (the owner is tearing down and may already have freed the
  // context): finished ones keep their status, queued ones are flushed.
  // Afterwards nothing touches their memory.
  for (auto it = g_ds.done.begin(); it != g_ds.done.end();) {
    if (it->voice_va == va && it->voice_serial == v->serial) {
      if (it->pkt.completed_size_va) rotk_wr32(it->pkt.completed_size_va, it->completed);
      if (it->pkt.status_va) rotk_wr32(it->pkt.status_va, it->status);
      it = g_ds.done.erase(it);
    } else {
      ++it;
    }
  }
  for (const Packet& p : v->packets) {
    if (p.completed_size_va) rotk_wr32(p.completed_size_va, 0);
    if (p.status_va) rotk_wr32(p.status_va, kPacketFlushed);
  }
  if (v->stream && v->started)
    rotk_log("dsound: stream 0x%08x released: %.3f s of audio played over %.3f s of mixer time "
            "(t %.3f..%.3f), %zu packets dropped",
            va, double(v->played_frames) / v->fmt.rate, v->last_play_s - v->first_play_s,
            v->first_play_s, v->last_play_s, v->packets.size());
  g_ds.voices.erase(va);
  DSLOG("release 0x%08x", va);
  delete v;
  rotk_wr32(va + 4, 0);
  g_free_objs.push_back(va);
  return 0;
}

// ---- API: DirectSound ------------------------------------------------------
// Each: [esp+4..] stdcall arguments.

// HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND8* ppDS, LPUNKNOWN)
void DirectSoundCreate(RotkCtx& c) {
  uint32_t pp = rotk_arg(c, 1);
  ensure_started();
  {
    std::lock_guard<std::mutex> lk(g_ds.mu);
    if (!g_ds_obj) {
      g_ds_obj = new_object(g_vt_ds, 0);
      rotk_log("dsound: DirectSoundCreate -> 0x%08x (HLE, no APU)", g_ds_obj);
    }
    ++g_ds_refs;
  }
  if (pp) rotk_wr32(pp, g_ds_obj);
  rotk_ret_stdcall(c, S_OK, 3);
}

// ULONG IDirectSound_Release(LPDIRECTSOUND8)
void IDirectSound_Release(RotkCtx& c) {
  std::lock_guard<std::mutex> lk(g_ds.mu);
  if (g_ds_refs > 0) --g_ds_refs;
  DSLOG("IDirectSound_Release -> %d", g_ds_refs);
  // The device stays (other owners may still hold buffers made from it).
  rotk_ret_stdcall(c, uint32_t(g_ds_refs), 1);
}

// HRESULT IDirectSound_DownloadEffectsImage(pDS, pvImage, dwSize, pImageLoc, ppImageDesc)
void IDirectSound_DownloadEffectsImage(RotkCtx& c) {
  uint32_t size = rotk_arg(c, 2), ppdesc = rotk_arg(c, 4);
  rotk_log("dsound: DownloadEffectsImage %u bytes (DSP effects not emulated)", size);
  if (ppdesc) {
    static uint32_t desc = rotk_hle_alloc(0x200);  // DSEFFECTIMAGEDESC, no effects
    rotk_wr32(ppdesc, desc);
  }
  rotk_ret_stdcall(c, S_OK, 5);
}

// HRESULT IDirectSound_SynchPlayback(pDS): starts together every buffer and
// stream waiting with DSBPLAY_SYNCHPLAYBACK / DSSTREAMPAUSE_SYNCHPLAYBACK
// (0x1470C3 walks the voices flagged 0x40 and starts them in one APU write).
void IDirectSound_SynchPlayback(RotkCtx& c) {
  std::lock_guard<std::mutex> lk(g_ds.mu);
  int n = 0;
  for (auto& [va, v] : g_ds.voices) {
    if (!v->sync_wait) continue;
    v->sync_wait = false;
    if (v->stream) v->paused = false;
    else v->playing = true;
    ++n;
  }
  DSLOG("SynchPlayback: %d voices", n);
  rotk_ret_stdcall(c, S_OK, 1);
}

// HRESULT IDirectSound_SetI3DL2Listener(pDS, LPCDSI3DL2LISTENER, dwApply)
void IDirectSound_SetI3DL2Listener(RotkCtx& c) {
  // DSI3DL2LISTENER: lRoom, lRoomHF, flRoomRolloffFactor, flDecayTime,
  // flDecayHFRatio, lReflections, flReflectionsDelay, lReverb, flReverbDelay,
  // flDiffusion, flDensity, flHFReference.
  if (uint32_t p = rotk_arg(c, 1)) {
    auto f = [&](int i) {
      uint32_t u = rotk_rd32(p + 4 * i);
      float x;
      std::memcpy(&x, &u, 4);
      return x;
    };
    auto l = [&](int i) { return int32_t(rotk_rd32(p + 4 * i)); };
    rotk_log("dsound: SetI3DL2Listener room %d roomHF %d rolloff %.2f decay %.2f s hfratio %.2f "
            "refl %d delay %.3f reverb %d delay %.3f diffusion %.0f density %.0f hfref %.0f "
            "(reverb not emulated)",
            l(0), l(1), f(2), f(3), f(4), l(5), f(6), l(7), f(8), f(9), f(10), f(11));
  }
  rotk_ret_stdcall(c, S_OK, 3);
}

// HRESULT IDirectSound_CreateSoundBuffer(pDS, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN)
void IDirectSound_CreateSoundBuffer(RotkCtx& c) {
  uint32_t out = 0, pp = rotk_arg(c, 2);
  uint32_t hr = create_buffer(rotk_arg(c, 1), &out);
  if (pp) rotk_wr32(pp, out);
  rotk_ret_stdcall(c, hr, 4);
}

// HRESULT DirectSoundCreateBuffer(LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*)
void DirectSoundCreateBuffer(RotkCtx& c) {
  uint32_t out = 0, pp = rotk_arg(c, 1);
  uint32_t hr = create_buffer(rotk_arg(c, 0), &out);
  if (pp) rotk_wr32(pp, out);
  rotk_ret_stdcall(c, hr, 2);
}

// HRESULT DirectSoundCreateStream(LPCDSSTREAMDESC, LPDIRECTSOUNDSTREAM*)
void DirectSoundCreateStream(RotkCtx& c) {
  uint32_t out = 0, pp = rotk_arg(c, 1);
  uint32_t hr = create_stream(rotk_arg(c, 0), &out);
  if (pp) rotk_wr32(pp, out);
  rotk_ret_stdcall(c, hr, 2);
}

// void DirectSoundDoWork(void)
void DirectSoundDoWork(RotkCtx& c) {
  deliver_completions(c);
  rotk_ret_cdecl(c, 0);
}

// ---- API: IDirectSoundBuffer -------------------------------------------------

// Runs fn on the buffer under the lock; unknown objects get E_FAIL.
template <typename F>
uint32_t with_voice(uint32_t va, const char* what, F fn) {
  std::lock_guard<std::mutex> lk(g_ds.mu);
  Voice* v = voice(va);
  if (!v) {
    rotk_log("dsound: %s on unknown object 0x%08x", what, va);
    return E_FAIL;
  }
  return fn(*v);
}

void IDirectSoundBuffer_Release(RotkCtx& c) {
  rotk_ret_stdcall(c, release_voice(rotk_arg(c, 0)), 1);
}

void IDirectSoundBuffer_SetVolume(RotkCtx& c) {
  int32_t vol = int32_t(rotk_arg(c, 1));
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetVolume", [&](Voice& v) {
                    v.volume = vol < -10000 ? -10000 : vol > 0 ? 0 : vol;
                    DSLOG("SetVolume 0x%08x %d", v.va, v.volume);
                    return S_OK;
                  }), 2);
}

void IDirectSoundBuffer_SetFrequency(RotkCtx& c) {
  uint32_t hz = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetFrequency", [&](Voice& v) {
                    v.freq = hz;
                    DSLOG("SetFrequency 0x%08x %u", v.va, hz);
                    return S_OK;
                  }), 2);
}

// HRESULT SetFilter(pBuffer, LPCDSFILTERDESC): the per-voice DSP filter.
void IDirectSoundBuffer_SetFilter(RotkCtx& c) {
  uint32_t d = rotk_arg(c, 1);
  if (d)
    DSLOG("SetFilter 0x%08x mode %u q %u coef %08x %08x %08x %08x (not emulated)", rotk_arg(c, 0),
          rotk_rd32(d), rotk_rd32(d + 4), rotk_rd32(d + 8), rotk_rd32(d + 12), rotk_rd32(d + 16),
          rotk_rd32(d + 20));
  rotk_ret_stdcall(c, S_OK, 2);
}

void IDirectSoundBuffer_SetMixBins(RotkCtx& c) {
  uint32_t mb = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetMixBins", [&](Voice& v) {
                    apply_mixbins(v, mb, true);
                    DSLOG("SetMixBins 0x%08x: %d bins, first %u", v.va, v.nbins, v.bins[0]);
                    return S_OK;
                  }), 2);
}

void IDirectSoundBuffer_SetMixBinVolumes(RotkCtx& c) {
  uint32_t mb = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetMixBinVolumes", [&](Voice& v) {
                    apply_mixbins(v, mb, false);
                    DSLOG("SetMixBinVolumes 0x%08x FL %d FR %d C %d BL %d BR %d", v.va, v.bin_vol[0],
                          v.bin_vol[1], v.bin_vol[2], v.bin_vol[4], v.bin_vol[5]);
                    return S_OK;
                  }), 2);
}

uint32_t region_end(const Voice& v) {
  uint32_t end = v.region_len ? v.region_start + v.region_len : v.data_bytes;
  return end > v.data_bytes ? v.data_bytes : end;
}

// HRESULT Play(pBuffer, dwReserved1, dwReserved2, dwFlags)
void IDirectSoundBuffer_Play(RotkCtx& c) {
  uint32_t flags = rotk_arg(c, 3);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "Play", [&](Voice& v) {
                    v.looping = flags & 1;  // DSBPLAY_LOOPING
                    double start = v.fmt.bytes_to_frames(v.region_start);
                    double end = v.fmt.bytes_to_frames(region_end(v));
                    if ((flags & 2) || v.pos < start || v.pos >= end) v.pos = start;  // FROMSTART
                    if (test_clock_frames() && (flags & 1) && !v.playing)
                      rotk_log("dsound: test clock: looping Play 0x%08x at vblank %llu, mixed %llu",
                              v.va, (unsigned long long)rotk_vblank_count(),
                              (unsigned long long)g_ds.mixed_frames);
                    v.sync_wait = flags & 4;  // DSBPLAY_SYNCHPLAYBACK
                    if (!v.playing) eg_start(v), v.skip_muted = false;  // VOICE_ON starts the envelope
                    v.playing = !v.sync_wait;
                    DSLOG("Play 0x%08x flags=%u pos=%.0f data=%u", v.va, flags, v.pos, v.data_bytes);
                    return S_OK;
                  }), 4);
}

}  // namespace

// Movie skipped (movies.cpp): mute the looping 44.1 kHz buffers playing now - the movie's
// soundtrack ring pair. Returns how many.
extern "C" int rotk_dsound_mute_movie_rings() {
  int n = 0;
  std::string names;
  {
    std::lock_guard<std::mutex> lk(g_ds.mu);
    for (auto& [va, v] : g_ds.voices) {
      if (!v->playing || !v->looping || v->stream || (v->freq ? v->freq : v->fmt.rate) != 44100) continue;
      v->skip_muted = true, ++n;
      char b[16];
      std::snprintf(b, sizeof b, "%s0x%08x", names.empty() ? "" : ",", va);
      names += b;
    }
  }
  rotk_log("dsound: movie skipped: muted %d soundtrack ring voice(s) %s", n, names.c_str());
  return n;
}

namespace {

void IDirectSoundBuffer_Stop(RotkCtx& c) {
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "Stop", [&](Voice& v) {
                    v.playing = v.sync_wait = false;
                    return S_OK;
                  }), 1);
}

// HRESULT SetPlayRegion(pBuffer, dwPlayStart, dwPlayLength) (bytes; 0 = to the end)
void IDirectSoundBuffer_SetPlayRegion(RotkCtx& c) {
  uint32_t start = rotk_arg(c, 1), len = rotk_arg(c, 2);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetPlayRegion", [&](Voice& v) {
                    if (len ? start + len > v.data_bytes : start > v.data_bytes)
                      return DSERR_INVALIDCALL;
                    v.region_start = start;
                    v.region_len = len;
                    return S_OK;
                  }), 3);
}

// HRESULT GetStatus(pBuffer, LPDWORD): DSBSTATUS_PLAYING 1 | DSBSTATUS_LOOPING 4.
void IDirectSoundBuffer_GetStatus(RotkCtx& c) {
  uint32_t out = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "GetStatus", [&](Voice& v) {
                    if (out) rotk_wr32(out, v.playing ? (v.looping ? 5u : 1u) : 0u);
                    return S_OK;
                  }), 2);
}

// HRESULT GetCurrentPosition(pBuffer, LPDWORD pdwPlay, LPDWORD pdwWrite) (bytes)
void IDirectSoundBuffer_GetCurrentPosition(RotkCtx& c) {
  uint32_t pplay = rotk_arg(c, 1), pwrite = rotk_arg(c, 2);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "GetCurrentPosition", [&](Voice& v) {
                    // The game feeds its looping rings from a 10 ms poll: a long gap
                    // between polls of a playing ring means its feeder stalled
                    // (the ring is only ~50 ms).
                    if (v.playing && v.looping) {
                      uint64_t now = g_ds.mixed_frames;
                      if (v.poll_frame && now - v.poll_frame > kOutRate / 25)
                        rotk_log("dsound: ring 0x%08x not polled for %.0f ms (at %.2f s, ring %.0f ms)",
                                v.va, double(now - v.poll_frame) * 1000 / kOutRate,
                                double(now) / kOutRate,
                                double(v.fmt.bytes_to_frames(v.data_bytes)) * 1000 / v.fmt.rate);
                      v.poll_frame = now;
                    }
                    uint32_t play = v.fmt.frames_to_bytes(uint32_t(v.pos));
                    uint32_t write = play;
                    if (v.playing && v.data_bytes) {
                      uint32_t lead = v.fmt.frames_to_bytes(
                          std::max<uint32_t>(32, v.fmt.frames_per_block()));
                      write = (play + lead) % v.data_bytes;
                    }
                    if (pplay) rotk_wr32(pplay, play);
                    if (pwrite) rotk_wr32(pwrite, write);
                    DSLOG("GetCurrentPosition 0x%08x play %u write %u", v.va, play, write);
                    return S_OK;
                  }), 3);
}

void IDirectSoundBuffer_SetCurrentPosition(RotkCtx& c) {
  uint32_t bytes = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetCurrentPosition", [&](Voice& v) {
                    v.pos = v.fmt.bytes_to_frames(bytes > v.data_bytes ? 0 : bytes);
                    return S_OK;
                  }), 2);
}

// HRESULT SetBufferData(pBuffer, LPVOID pvBufferData, DWORD dwBufferBytes)
void IDirectSoundBuffer_SetBufferData(RotkCtx& c) {
  uint32_t data = rotk_arg(c, 1), bytes = rotk_arg(c, 2);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetBufferData", [&](Voice& v) {
                    v.data_va = data;
                    v.data_bytes = data ? bytes : 0;
                    v.region_start = v.region_len = v.loop_start = v.loop_len = 0;
                    if (v.pos >= v.fmt.bytes_to_frames(v.data_bytes)) v.pos = 0;
                    v.cache_block = -1;
                    v.skip_muted = false;
                    if (!v.data_bytes) v.playing = false;
                    DSLOG("SetBufferData 0x%08x data=0x%08x bytes=%u", v.va, data, bytes);
                    return S_OK;
                  }), 3);
}

// HRESULT StopEx(pBuffer, REFERENCE_TIME rtTimeStamp, DWORD dwFlags): DSBSTOPEX_ENVELOPE (1)
// runs the release of the amplitude envelope, then the voice stops; DSBSTOPEX_RELEASEWAVEFORM
// (2) would play out the loop and stops at once here. Time stamps: now.
void IDirectSoundBuffer_StopEx(RotkCtx& c) {
  uint32_t flags = rotk_arg(c, 3);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "StopEx", [&](Voice& v) {
                    if ((flags & 1) && v.playing) {
                      eg_release(v);
                    } else {
                      v.playing = false;
                    }
                    v.sync_wait = false;
                    DSLOG("StopEx 0x%08x flags=%u", v.va, flags);
                    return S_OK;
                  }), 4);
}

// HRESULT SetFormat(pBuffer, LPCWAVEFORMATEX)
void IDirectSoundBuffer_SetFormat(RotkCtx& c) {
  uint32_t wfx = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetFormat", [&](Voice& v) {
                    if (!wfx) return DSERR_INVALIDCALL;
                    v.fmt = read_wfx(wfx);
                    v.cache_block = -1;
                    DSLOG("SetFormat 0x%08x tag=0x%x ch=%u rate=%u", v.va, v.fmt.tag, v.fmt.channels,
                          v.fmt.rate);
                    return S_OK;
                  }), 2);
}

// HRESULT SetLoopRegion(pBuffer, dwLoopStart, dwLoopLength) (bytes; 0 = to the end)
void IDirectSoundBuffer_SetLoopRegion(RotkCtx& c) {
  uint32_t start = rotk_arg(c, 1), len = rotk_arg(c, 2);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetLoopRegion", [&](Voice& v) {
                    if (start > v.data_bytes || (len && start + len > v.data_bytes))
                      return DSERR_INVALIDCALL;
                    v.loop_start = start;
                    v.loop_len = len;
                    return S_OK;
                  }), 3);
}

// HRESULT SetHeadroom(pBuffer, DWORD dwHeadroom) (mB)
void IDirectSoundBuffer_SetHeadroom(RotkCtx& c) {
  int32_t hr_mb = int32_t(rotk_arg(c, 1));
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetHeadroom", [&](Voice& v) {
                    v.headroom = hr_mb;
                    return S_OK;
                  }), 2);
}

// HRESULT SetEG(pBuffer, LPCDSENVELOPEDESC {dwEG, dwMode, dwDelay, dwAttack, dwHold, dwDecay,
// dwRelease, dwSustain, lPitchScale, lFilterCutOff}). dwEG 1 = the amplitude envelope: the
// library (0x142CAA) writes the 12-bit times and the 8-bit sustain unscaled into the voice's
// ENV0/ENVA/LFO_ENV registers; the mixer runs it as the VP does. dwEG 0 (pitch/filter
// envelope) is not emulated (this title only sets the amplitude one, 0x813B0).
void IDirectSoundBuffer_SetEG(RotkCtx& c) {
  uint32_t d = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "SetEG", [&](Voice& v) {
                    if (!d) return DSERR_INVALIDCALL;
                    if (rotk_rd32(d) != 1) {
                      static std::atomic<bool> once{false};
                      if (!once.exchange(true)) rotk_log("dsound: SetEG: multi (pitch/filter) envelope not emulated");
                      return S_OK;
                    }
                    Voice::Envelope& e = v.eg;
                    e.mode = rotk_rd32(d + 4) & 7;
                    e.delay = rotk_rd32(d + 8) & 0xfff;
                    e.attack = rotk_rd32(d + 12) & 0xfff;
                    e.hold = rotk_rd32(d + 16) & 0xfff;
                    e.decay = rotk_rd32(d + 20) & 0xfff;
                    e.release = rotk_rd32(d + 24) & 0xfff;
                    e.sustain = rotk_rd32(d + 28) & 0xff;
                    if (v.playing) eg_start(v);
                    DSLOG("SetEG 0x%08x mode %u delay %u attack %u hold %u decay %u release %u sustain %u",
                          v.va, e.mode, e.delay, e.attack, e.hold, e.decay, e.release, e.sustain);
                    return S_OK;
                  }), 2);
}

// HRESULT IDirectSound_CommitDeferredSettings(pDS): settings apply immediately here.
void IDirectSound_CommitDeferredSettings(RotkCtx& c) { rotk_ret_stdcall(c, S_OK, 1); }

// ---- API: IDirectSoundStream -------------------------------------------------

// HRESULT IDirectSoundStream_Pause(pStream, DWORD dwPause): 0 RESUME, 1 PAUSE,
// 2 SYNCHPLAYBACK (paused until IDirectSound_SynchPlayback), 3 PAUSENOACTIVATE.
void IDirectSoundStream_Pause(RotkCtx& c) {
  uint32_t mode = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "Stream_Pause", [&](Voice& v) {
                    v.paused = mode != 0;
                    v.sync_wait = mode == 2;
                    DSLOG("Stream_Pause 0x%08x mode=%u", v.va, mode);
                    return S_OK;
                  }), 2);
}

// HRESULT IDirectSoundStream_FlushEx(pStream, REFERENCE_TIME rt, DWORD dwFlags)
// Flushed packets complete (FLUSHED, callbacks/events) before the call
// returns, as in the XDK; after that the stream holds no game packet.
void IDirectSoundStream_FlushEx(RotkCtx& c) {
  uint32_t hr = with_voice(rotk_arg(c, 0), "Stream_FlushEx", [&](Voice& v) {
    flush_stream(v);
    return S_OK;
  });
  deliver_completions(c);
  rotk_ret_stdcall(c, hr, 4);
}

// XMediaObject vtable (this in [esp+4]).
void Stream_AddRef(RotkCtx& c) {
  uint32_t r = with_voice(rotk_arg(c, 0), "Stream_AddRef", [&](Voice& v) { return uint32_t(++v.refs); });
  rotk_ret_stdcall(c, r, 1);
}
void Stream_Release(RotkCtx& c) { rotk_ret_stdcall(c, release_voice(rotk_arg(c, 0)), 1); }

// HRESULT GetInfo(this, LPXMEDIAINFO {dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead})
void Stream_GetInfo(RotkCtx& c) {
  uint32_t info = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "Stream_GetInfo", [&](Voice& v) {
                    if (info) {
                      rotk_wr32(info, 0x1 | 0x4);  // FIXED_SAMPLE_SIZE | INPUT_ASYNC
                      rotk_wr32(info + 4, v.fmt.block_align);
                      rotk_wr32(info + 8, 0);
                      rotk_wr32(info + 12, 0x1000);
                    }
                    return S_OK;
                  }), 2);
}

// HRESULT GetStatus(this, LPDWORD): DSSTREAMSTATUS_READY 1, PLAYING 0x10000,
// PAUSED 0x20000, STARVED 0x40000.
void Stream_GetStatus(RotkCtx& c) {
  uint32_t out = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "Stream_GetStatus", [&](Voice& v) {
                    uint32_t s = 0;
                    if (v.packets.size() < v.max_packets) s |= 1;
                    if (v.paused) s |= 0x20000;
                    else if (!v.packets.empty()) s |= 0x10000;
                    else if (v.started) s |= 0x40000;
                    if (out) rotk_wr32(out, s);
                    return S_OK;
                  }), 2);
}

// HRESULT Process(this, LPCXMEDIAPACKET pInput, LPCXMEDIAPACKET pOutput)
// XMEDIAPACKET {pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus,
//               hCompletionEvent|pContext, prtTimestamp}
void Stream_Process(RotkCtx& c) {
  uint32_t in = rotk_arg(c, 1);
  rotk_ret_stdcall(c, with_voice(rotk_arg(c, 0), "Stream_Process", [&](Voice& v) {
                    if (!in) return DSERR_INVALIDCALL;
                    Packet p;
                    p.buffer_va = rotk_rd32(in);
                    p.bytes = rotk_rd32(in + 4);
                    p.completed_size_va = rotk_rd32(in + 8);
                    p.status_va = rotk_rd32(in + 12);
                    p.context = rotk_rd32(in + 16);
                    if (p.status_va) rotk_wr32(p.status_va, kPacketPending);
                    if (p.completed_size_va) rotk_wr32(p.completed_size_va, 0);
                    v.packets.push_back(p);
                    v.started = true;
                    DSLOG("Stream_Process 0x%08x buf=0x%08x bytes=%u queued=%zu", v.va, p.buffer_va,
                          p.bytes, v.packets.size());
                    return S_OK;
                  }), 3);
}

// HRESULT Discontinuity(this): end of data; the queue just drains.
void Stream_Discontinuity(RotkCtx& c) { rotk_ret_stdcall(c, S_OK, 1); }

void Stream_Flush(RotkCtx& c) {
  uint32_t hr = with_voice(rotk_arg(c, 0), "Stream_Flush", [&](Voice& v) {
    flush_stream(v);
    return S_OK;
  });
  deliver_completions(c);
  rotk_ret_stdcall(c, hr, 1);
}

// ---- registration ------------------------------------------------------------

struct Api {
  uint32_t va;
  HostFn fn;
  const char* name;
};
// Identities and evidence: FUNCTIONS.md.
// Entry points of this title (EA-062, XDK 5558): every DSOUND entry the game code calls
// (Ghidra callers in .text), each identified by its call site in the game's sound module
// (0x80DC0..0x81C60: arguments and use) and the OOVPA name of its wrapper (2026-11-02).
const Api kApi[] = {
    {0x140750, DirectSoundCreate, "DirectSoundCreate"},
    {0x13F5AE, DirectSoundDoWork, "DirectSoundDoWork"},
    {0x13E9C2, IDirectSound_Release, "IDirectSound_Release"},
    {0x13F477, IDirectSound_DownloadEffectsImage, "IDirectSound_DownloadEffectsImage"},
    {0x14058A, IDirectSound_SetI3DL2Listener, "IDirectSound_SetI3DL2Listener"},
    {0x140566, IDirectSound_CreateSoundBuffer, "IDirectSound_CreateSoundBuffer"},
    {0x14038F, IDirectSound_CommitDeferredSettings, "IDirectSound_CommitDeferredSettings"},
    {0x13E9D8, IDirectSoundBuffer_Release, "IDirectSoundBuffer_Release"},
    {0x13F49E, IDirectSoundBuffer_SetEG, "IDirectSoundBuffer_SetEG"},
    {0x13F4BA, IDirectSoundBuffer_SetHeadroom, "IDirectSoundBuffer_SetHeadroom"},
    {0x13F4D6, IDirectSoundBuffer_SetMixBinVolumes, "IDirectSoundBuffer_SetMixBinVolumes"},
    {0x13F4F2, IDirectSoundBuffer_Play, "IDirectSoundBuffer_Play"},
    {0x13F516, IDirectSoundBuffer_Stop, "IDirectSoundBuffer_Stop"},
    {0x13F52E, IDirectSoundBuffer_StopEx, "IDirectSoundBuffer_StopEx"},
    {0x13F552, IDirectSoundBuffer_SetLoopRegion, "IDirectSoundBuffer_SetLoopRegion"},
    {0x13F572, IDirectSoundBuffer_GetStatus, "IDirectSoundBuffer_GetStatus"},
    {0x13F58E, IDirectSoundBuffer_GetCurrentPosition, "IDirectSoundBuffer_GetCurrentPosition"},
    {0x13F9A0, IDirectSoundBuffer_SetFrequency, "IDirectSoundBuffer_SetFrequency"},
    {0x13F9BC, IDirectSoundBuffer_SetMixBins, "IDirectSoundBuffer_SetMixBins"},
    {0x13FF80, IDirectSoundBuffer_SetFormat, "IDirectSoundBuffer_SetFormat"},
    {0x13FF9C, IDirectSoundBuffer_SetBufferData, "IDirectSoundBuffer_SetBufferData"},
};

// DSOUND entries that may run as-is without a warning (CRT static initialisers of DSOUND
// globals). None located for this title yet: such calls log once through Guard and run.
const uint32_t kHarmless[] = {0};

template <uint32_t VA>
void Guard(RotkCtx& c) {
  static std::atomic<bool> seen{false};
  static const bool strict = env_on("ROTK_DSOUND_STRICT");
  if (!seen.exchange(true) || strict) {
    if (strict)
      rotk_fatal("dsound: DSOUND entry 0x%06x is not HLE'd (called from 0x%08x)", VA,
                rotk_rd32(c.esp));
    rotk_log("dsound: WARNING: DSOUND entry 0x%06x is not HLE'd (called from 0x%08x); "
            "running the original (it may touch APU hardware)", VA, rotk_rd32(c.esp));
  }
  rotk_original(VA)(c);
}

struct Registrar {
  Registrar() {
    std::set<uint32_t> done;
    for (const Api& a : kApi) {
      rotk_register_override_at_start(a.va, a.fn, a.name);
      done.insert(a.va);
    }
    for (uint32_t va : kHarmless) done.insert(va);
#define DS_ENTRY(va) \
  if (!done.count(va)) rotk_register_override_at_start(va, Guard<va>, "dsound_guard");
#include "hle/dsound/dsound_entries.inc"
#undef DS_ENTRY
  }
} g_registrar;

const HostFn kStreamVtbl[7] = {Stream_AddRef, Stream_Release, Stream_GetInfo, Stream_GetStatus,
                               Stream_Process, Stream_Discontinuity, Stream_Flush};

}  // namespace
