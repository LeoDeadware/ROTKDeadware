// DSound HLE mixer (owner: Audio): renders every playing buffer and stream
// into one stereo float stream at 48 kHz, in real time, independent of the
// game's 60 Hz frame clock.
//
// Clock: one mixer thread mixes against the wall clock (steady_clock), 5 ms at
// a time, and pushes into the SDL3 device stream. The device never sets the
// pace: a device (or SDL's dummy driver, ~6% slow) running off the wall clock
// only grows or drains its queue, which is kept bounded (excess dropped).
// Voice positions and packet completions advance only here, so
// GetCurrentPosition/GetStatus and XMV's audio clock follow real time.
//
// Env:
//   ROTK_AUDIO=0          no SDL device (timer thread only; positions still advance)
//   ROTK_AUDIO_WAV=path   also write the mix to a 16-bit stereo WAV (header kept valid)
//   ROTK_AUDIO_SUBMIX_HEADROOM=n   dev: mixbin headroom for every bin (default: DSOUND's 1)
//   ROTK_AUDIO_NO_EG=1    dev: ignore the amplitude envelopes (control runs)
//   ROTK_AUDIO_SOLO=pcm|adpcm|stream   dev: output only that class of voices (the
//                        others still play silently, so timing is unchanged);
//                        pcm = the game's music rings + PCM effects
// Tests: SDL_AUDIO_DRIVER=dummy + ROTK_AUDIO_WAV, never the speakers.

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "hle/dsound/adpcm.h"
#include "hle/dsound/drift.h"
#include "hle/dsound/limiter.h"
#include "hle/kernel/kernel_exports.h"
#include "hle/dsound/dsound_internal.h"
#include "runtime/rotk_runtime.h"

namespace rotkds {

namespace {

float pcm_sample(const Format& f, const uint8_t* p) {
  if (f.bits == 8) return (int(p[0]) - 128) / 128.f;
  return int16_t(p[0] | p[1] << 8) / 32768.f;
}

}  // namespace

void decode(const Format& f, const uint8_t* src, uint32_t bytes, std::vector<float>& out) {
  int ch = f.channels;
  if (f.tag == kFmtXboxAdpcm) {
    uint32_t blocks = bytes / f.block_align;
    out.resize(size_t(blocks) * kAdpcmBlockSamples * ch);
    for (uint32_t i = 0; i < blocks; ++i)
      adpcm_decode_block(src + size_t(i) * f.block_align, ch, &out[size_t(i) * kAdpcmBlockSamples * ch]);
    return;
  }
  uint32_t bps = f.bits == 8 ? 1 : 2;
  uint32_t frames = f.block_align ? bytes / f.block_align : 0;
  out.resize(size_t(frames) * ch);
  for (uint32_t i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) out[size_t(i) * ch + c] = pcm_sample(f, src + i * f.block_align + c * bps);
}

void flush_stream(Voice& v) {
  for (const Packet& p : v.packets) {
    Completion d;
    d.pkt = p;
    d.status = kPacketFlushed;
    d.completed = 0;
    d.callback = v.callback;
    d.cb_context = v.cb_context;
  d.voice_va = v.va;
  d.voice_serial = v.serial;
    d.voice_va = v.va;
    d.voice_serial = v.serial;
    g_ds.done.push_back(d);
  }
  v.packets.clear();
  v.pkt_decoded = false;
  v.pkt_pos = 0;
}

// ---- amplitude envelope (MCPX VP, as xemu's voice_step_envelope) ---------------
// One step per APU frame (32 samples at 48 kHz). Rates count 16 frames per unit:
// attack ramps 0 -> 255 linearly, hold stays at 255, decay falls exponentially
// (-64 dB over the whole rate) until the sustain level, release falls as
// e^(-6.91 t) over its rate and then turns the voice off.

namespace {
constexpr int kApuFrame = 32;
enum { kEgOff, kEgDelay, kEgAttack, kEgHold, kEgDecay, kEgSustain, kEgRelease };
const bool g_no_eg = [] {
  const char* s = getenv("ROTK_AUDIO_NO_EG");
  return s && atoi(s) != 0;
}();

}  // namespace

void eg_start(Voice& v) {
  v.eg_sub = 0;
  v.eg_count = 0;
  switch (v.eg.mode) {
    case kEgOff: v.eg_stage = kEgOff; v.eg_level = 1; return;
    case kEgDelay: v.eg_stage = kEgDelay; v.eg_count = v.eg.delay * 16; v.eg_level = 0; return;
    case kEgHold: v.eg_stage = kEgHold; v.eg_count = v.eg.hold * 16; v.eg_level = 1; return;
    case kEgDecay: v.eg_stage = kEgDecay; v.eg_count = v.eg.decay * 16; v.eg_level = 1; return;
    case kEgSustain: v.eg_stage = kEgSustain; v.eg_level = v.eg.sustain / 255.f; return;
    default: v.eg_stage = kEgAttack; v.eg_level = v.eg.attack ? 0.f : 1.f; return;
  }
}

void eg_release(Voice& v) {
  v.eg_release_from = v.eg_level;
  v.eg_stage = kEgRelease;
  v.eg_count = v.eg.release * 16;
  if (!v.eg.release || g_no_eg) v.playing = false;
}

namespace {

void eg_step(Voice& v) {
  const Voice::Envelope& e = v.eg;
  switch (v.eg_stage) {
    case kEgDelay:
      v.eg_level = 0;
      if (v.eg_count) --v.eg_count;
      if (!v.eg_count) v.eg_stage = kEgAttack;
      return;
    case kEgAttack:
      if (!e.attack || ++v.eg_count >= e.attack * 16) {
        v.eg_level = 1;
        v.eg_stage = kEgHold;
        v.eg_count = e.hold * 16;
      } else {
        v.eg_level = float(v.eg_count) / float(e.attack * 16);
      }
      return;
    case kEgHold:
      v.eg_level = 1;
      if (v.eg_count) --v.eg_count;
      if (!v.eg_count) v.eg_stage = kEgDecay, v.eg_count = e.decay * 16;
      return;
    case kEgDecay: {
      float x = 0;
      if (e.decay) {
        if (v.eg_count) --v.eg_count;
        x = 255.f * std::pow(0.99988799f, float(e.decay * 16 - v.eg_count) * 4096.f / float(e.decay));
      }
      if (x <= e.sustain + 0.2f || !v.eg_count) {
        v.eg_stage = kEgSustain;
        x = float(e.sustain);
      }
      v.eg_level = x / 255.f;
      return;
    }
    case kEgSustain: v.eg_level = e.sustain / 255.f; return;
    case kEgRelease: {
      if (v.eg_count) --v.eg_count;
      float t = e.release ? 1.f - float(v.eg_count) / float(e.release * 16) : 1.f;
      v.eg_level = std::exp(-6.91f * t) * v.eg_release_from;
      if (!v.eg_count) v.playing = false;
      return;
    }
    default: v.eg_level = 1; return;
  }
}

// Advances the envelope by one output sample; returns its level.
inline float eg_tick(Voice& v) {
  if (v.eg_stage == kEgOff || g_no_eg) return 1;
  if (++v.eg_sub >= kApuFrame) {
    v.eg_sub = 0;
    eg_step(v);
  }
  return v.eg_level;
}

float mb_gain(int32_t mb) { return mb <= -10000 ? 0.f : std::pow(10.f, mb / 2000.f); }

// Stereo fold-down of the mixbins, {to left, to right} per DSMIXBIN id; LFE
// and the effect sends dropped. itu is the hardware's: the EP (encode DSP)
// stereo downmix module (DSOUND chunk 0x1ED0F8) sums L + .7071 C + .7071 Ls
// (R alike), no normalisation, saturating at full scale (NOTES.md "Gain
// staging"). Other ROTK_AUDIO_FOLD values are diagnostics: front = FL/FR only,
// half = every bin at 0.5, sum = every bin at 1.
const float kFoldItu[11][2] = {{1, 0},       {0, 1},       {.7071f, .7071f}, {0, 0},
                               {.7071f, 0},  {0, .7071f},  // speakers FL FR C LFE BL BR
                               {1, 0},       {0, 1},       {.7071f, 0},      {0, .7071f},  // XTLK
                               {0, 0}};                    // I3DL2 send
float kFold[11][2];
const bool g_fold_init = [] {
  std::string m = getenv("ROTK_AUDIO_FOLD") ? getenv("ROTK_AUDIO_FOLD") : "itu";
  for (int b = 0; b < 11; ++b)
    for (int c = 0; c < 2; ++c) {
      float k = kFoldItu[b][c];
      bool front = b == 0 || b == 1 || b == 6 || b == 7;
      if (m == "front") k = front ? k : 0;
      else if (m == "half") k = k > 0 ? .5f : 0;
      else if (m == "sum") k = k > 0 ? 1 : 0;
      kFold[b][c] = k;
    }
  if (m != "itu") rotk_log("dsound: ROTK_AUDIO_FOLD=%s", m.c_str());
  return true;
}();

// Mixbin (submix) headroom, in 6 dB steps: the VP divides each bin's input by
// 2^headroom (NV1BA0_PIO_SET_SUBMIX_HEADROOM; xemu vp.c does the same). DSOUND
// programs it at init from its settings (ctor 0x13FFBC: 1 for bins 0..30, 0 for
// bin 31; APU init 0x1418EF -> 0x141460 writes them to 0xFE820200 + 4 * bin) and
// changes it only through IDirectSound_SetMixBinHeadroom, which this game never
// calls. Neither the GP executive (it DMAs the 6 bins to the EP as-is) nor the
// EP downmix applies make-up gain (its optional scale is off in the default
// EP config block, 0x1ED420 word 16 = 0).
const int g_submix_headroom = [] {
  const char* s = getenv("ROTK_AUDIO_SUBMIX_HEADROOM");
  return s ? std::clamp(atoi(s), 0, 7) : -1;
}();
inline float submix_gain(int bin) {
  int h = g_submix_headroom >= 0 ? g_submix_headroom : bin == 31 ? 0 : 1;
  return 1.f / float(1 << h);
}

struct Gains {
  float l[2], r[2];  // per source channel (0, 1)
};

// Per (bin, channel) path: volume - headroom + mixbin volume, in mB.
Gains voice_gains(const Voice& v) {
  Gains g{};
  int ch = v.fmt.channels > 1 ? 2 : 1;
  for (int i = 0; i < v.nbins; ++i) {
    int bin = v.bins[i];
    if (bin > 10) continue;
    int c = i % (v.fmt.channels ? v.fmt.channels : 1);
    if (c >= ch) continue;
    float a = mb_gain(v.volume - v.headroom + v.bin_vol[bin]) * submix_gain(bin);
    g.l[c] += a * kFold[bin][0];
    g.r[c] += a * kFold[bin][1];
  }
  if (ch == 1) {
    g.l[1] = g.l[0];
    g.r[1] = g.r[0];
  }
  return g;
}

// Frame `idx` of a buffer, channel ch (0 beyond the data).
float buffer_sample(Voice& v, uint32_t idx, int ch) {
  const Format& f = v.fmt;
  if (f.tag == kFmtXboxAdpcm) {
    uint32_t blk = idx / kAdpcmBlockSamples;
    if ((blk + 1) * f.block_align > v.data_bytes) return 0;
    if (v.cache_block != int64_t(blk)) {
      v.cache.resize(kAdpcmBlockSamples * f.channels);
      rotkds::adpcm_decode_block(static_cast<const uint8_t*>(rotk_ptr(v.data_va + blk * f.block_align)),
                         f.channels, v.cache.data());
      v.cache_block = blk;
    }
    return v.cache[(idx % kAdpcmBlockSamples) * f.channels + ch];
  }
  uint32_t off = idx * f.block_align;
  if (off + f.block_align > v.data_bytes) return 0;
  return pcm_sample(f, static_cast<const uint8_t*>(rotk_ptr(v.data_va + off)) + ch * (f.bits == 8 ? 1 : 2));
}

double voice_step(const Voice& v) {
  double hz = v.freq ? v.freq : v.fmt.rate;
  return hz / kOutRate;
}

void mix_buffer(Voice& v, float* out, int frames) {
  if (!v.data_va || !v.data_bytes || !v.fmt.block_align) {
    v.playing = false;
    return;
  }
  const Format& f = v.fmt;
  uint32_t end_b = v.region_len ? v.region_start + v.region_len : v.data_bytes;
  if (end_b > v.data_bytes) end_b = v.data_bytes;
  double start = f.bytes_to_frames(v.region_start), end = f.bytes_to_frames(end_b);
  if (end <= start) {
    v.playing = false;
    return;
  }
  // Looping repeats the loop region (default: the whole play region).
  double lstart = start + f.bytes_to_frames(v.loop_start);
  double lend = v.loop_len ? std::min(end, lstart + f.bytes_to_frames(v.loop_len)) : end;
  if (lstart >= lend) lstart = start, lend = end;
  if (v.looping) end = lend;
  Gains g = voice_gains(v);
  double step = voice_step(v);
  int ch = f.channels > 1 ? 2 : 1;
  for (int i = 0; i < frames; ++i) {
    uint32_t i0 = uint32_t(v.pos);
    float t = float(v.pos - i0);
    uint32_t i1 = i0 + 1;
    if (i1 >= end) i1 = v.looping ? uint32_t(lstart) : i0;
    float l = buffer_sample(v, i0, 0) * (1 - t) + buffer_sample(v, i1, 0) * t;
    float r = ch == 2 ? buffer_sample(v, i0, 1) * (1 - t) + buffer_sample(v, i1, 1) * t : 0;
    v.src_sumsq += double(l) * l;
    ++v.src_frames;
    float eg = eg_tick(v);
    if (!v.playing) return;  // end of the release
    l *= eg;
    r *= eg;
    out[2 * i] += l * g.l[0] + r * g.l[1] * (ch - 1);
    out[2 * i + 1] += l * g.r[0] + r * g.r[1] * (ch - 1);
    v.pos += step;
    if (v.pos >= end) {
      if (v.looping) {
        v.pos = lstart + std::fmod(v.pos - end, lend - lstart);
      } else {
        v.playing = false;
        v.pos = start;
        return;
      }
    }
  }
}

void complete_head(Voice& v) {
  Completion d;
  d.pkt = v.packets.front();
  d.status = kPacketSuccess;
  d.completed = d.pkt.bytes;
  d.callback = v.callback;
  d.cb_context = v.cb_context;
  d.voice_va = v.va;
  d.voice_serial = v.serial;
  g_ds.done.push_back(d);
  v.packets.pop_front();
  v.pkt_decoded = false;
}

void mix_stream(Voice& v, float* out, int frames) {
  Gains g = voice_gains(v);
  double step = voice_step(v);
  int ch = v.fmt.channels > 1 ? 2 : 1, src_ch = v.fmt.channels;
  for (int i = 0; i < frames && !v.packets.empty(); ++i) {
    if (!v.pkt_decoded) {
      const Packet& p = v.packets.front();
      v.pkt_pcm.clear();
      if (p.buffer_va && p.bytes)
        decode(v.fmt, static_cast<const uint8_t*>(rotk_ptr(p.buffer_va)), p.bytes, v.pkt_pcm);
      v.pkt_decoded = true;
      v.pkt_pos = std::max(0.0, v.pkt_pos);
    }
    size_t n = src_ch ? v.pkt_pcm.size() / src_ch : 0;
    if (v.pkt_pos >= n) {
      v.played_frames += n;
      v.pkt_pos -= n;
      complete_head(v);
      --i;
      continue;
    }
    size_t i0 = size_t(v.pkt_pos);
    if (v.first_play_s < 0) v.first_play_s = double(g_ds.mixed_frames + i) / kOutRate;
    v.last_play_s = double(g_ds.mixed_frames + i) / kOutRate;
    float l = v.pkt_pcm[i0 * src_ch];
    float r = ch == 2 ? v.pkt_pcm[i0 * src_ch + 1] : 0;
    out[2 * i] += l * g.l[0] + r * g.l[1] * (ch - 1);
    out[2 * i + 1] += l * g.r[0] + r * g.r[1] * (ch - 1);
    v.pkt_pos += step;
  }
}

// ---- output: stats, WAV tee, SDL / timer ---------------------------------------

struct Stats {
  double peak = 0, sumsq = 0;
  uint64_t frames = 0, clipped = 0, loud = 0;
  uint64_t post = 0;     // samples still over 0 dBFS after the limiter (should stay 0)
  uint64_t reduced = 0;  // frames the limiter turned down by more than 0.1 dB
  double min_gain = 1;   // deepest limiter gain
} g_stats, g_win;  // whole run; the current 10 s window
size_t g_win_voices = 0;  // most voices playing at once in the window
std::atomic<uint64_t> g_dropped{0};  // frames the device queue had no room for
std::atomic<double> g_device_ratio{1.0};  // last drift-control ratio

FILE* g_wav = nullptr;
uint64_t g_wav_frames = 0;

void wav_header(FILE* f, uint64_t frames) {
  uint32_t data = uint32_t(frames * 4);
  uint8_t h[44];
  auto w32 = [&](int o, uint32_t v) { std::memcpy(h + o, &v, 4); };
  auto w16 = [&](int o, uint16_t v) { std::memcpy(h + o, &v, 2); };
  std::memcpy(h, "RIFF", 4);
  w32(4, 36 + data);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  w32(16, 16);
  w16(20, 1);
  w16(22, 2);
  w32(24, kOutRate);
  w32(28, kOutRate * 4);
  w16(32, 4);
  w16(34, 16);
  std::memcpy(h + 36, "data", 4);
  w32(40, data);
  long pos = ftell(f);
  fseek(f, 0, SEEK_SET);
  fwrite(h, 1, 44, f);
  if (pos > 44) fseek(f, pos, SEEK_SET);
  fflush(f);
}

void log_window() {
  double rms = g_win.frames ? std::sqrt(g_win.sumsq / (2.0 * g_win.frames)) : 0;
  rotk_log("dsound: mixer window %.0f-%.0f s: peak %.3f, rms %.4f, limited %llu, after limiter %llu, "
          "limiter %.2f s down to %.1f dB, max voices %zu",
          double(g_stats.frames - g_win.frames) / kOutRate, double(g_stats.frames) / kOutRate, g_win.peak,
          rms, (unsigned long long)g_win.clipped, (unsigned long long)g_win.post,
          double(g_win.reduced) / kOutRate, 20 * std::log10(g_win.min_gain), g_win_voices);
  g_win = Stats{};
  g_win_voices = 0;
}

void log_stats(const char* when) {
  double secs = double(g_stats.frames) / kOutRate;
  double rms = g_stats.frames ? std::sqrt(g_stats.sumsq / (2.0 * g_stats.frames)) : 0;
  size_t voices, playing = 0;
  {
    std::lock_guard<std::mutex> lk(g_ds.mu);
    voices = g_ds.voices.size();
    for (auto& [va, v] : g_ds.voices) {
      if (v->playing || (!v->packets.empty() && !v->paused)) ++playing;
      if (v->src_frames && v->playing && !v->stream) {
        std::string bins;
        for (int b = 0; b < v->nbins; ++b)
          bins += (b ? "," : "") + std::to_string(v->bins[b]) + ":" + std::to_string(v->bin_vol[v->bins[b]]);
        rotk_log("dsound:   voice 0x%08x fmt 0x%x %u Hz vol %d headroom %d bins %s: source rms %.4f%s",
                va, v->fmt.tag, v->freq ? v->freq : v->fmt.rate, v->volume, v->headroom, bins.c_str(),
                std::sqrt(v->src_sumsq / v->src_frames), v->skip_muted ? " (muted: movie skipped)" : "");
        v->src_sumsq = 0;
        v->src_frames = 0;
      }
    }
  }
  rotk_log("dsound: mixer %s: %.1f s mixed, peak %.3f, rms %.4f, over 0 dBFS (limited) %llu, after limiter %llu, "
          "limiter %.1f s down to %.1f dB, non-silent %.1f s, "
          "voices %zu (%zu playing), device dropped %.2f s, device ratio %.5f",
          when, secs, g_stats.peak, rms, (unsigned long long)g_stats.clipped, (unsigned long long)g_stats.post,
          double(g_stats.reduced) / kOutRate, 20 * std::log10(g_stats.min_gain),
          double(g_stats.loud) / kOutRate, voices, playing, double(g_dropped) / kOutRate, g_device_ratio.load());
}

// Master limiter (limiter.h): the over-0-dBFS count ("limited") is taken before it.
rotkds::Limiter g_lim{kOutRate};

// ROTK_AUDIO_SOLO: 0 all, 1 PCM buffers, 2 ADPCM buffers, 3 streams.
const int g_solo = [] {
  const char* s = getenv("ROTK_AUDIO_SOLO");
  if (!s) return 0;
  std::string v = s;
  return v == "pcm" ? 1 : v == "adpcm" ? 2 : v == "stream" ? 3 : 0;
}();

bool audible(const Voice& v) {
  if (v.skip_muted) return false;
  if (!g_solo) return true;
  int cls = v.stream ? 3 : v.fmt.tag == kFmtXboxAdpcm ? 2 : 1;
  return cls == g_solo;
}

void mix(float* out, int frames) {
  std::memset(out, 0, sizeof(float) * 2 * frames);
  static std::vector<float> muted;
  muted.resize(size_t(frames) * 2);
  {
    std::lock_guard<std::mutex> lk(g_ds.mu);
    size_t n = 0;
    for (auto& [va, v] : g_ds.voices) n += v->playing || (v->stream && !v->paused && !v->packets.empty());
    g_win_voices = std::max(g_win_voices, n);
    for (auto& [va, v] : g_ds.voices) {
      float* dst = audible(*v) ? out : muted.data();
      if (v->stream) {
        if (!v->paused && !v->packets.empty()) mix_stream(*v, dst, frames);
      } else if (v->playing) {
        mix_buffer(*v, dst, frames);
      }
    }
    g_ds.mixed_frames += frames;
  }
  // DC blocker (one pole, ~10 Hz), as the console's AC-coupled output: the
  // game pauses one-shots by dropping them to the 188 Hz minimum frequency,
  // which turns them into near-DC that would otherwise sit in the mix.
  static float dc_x[2], dc_y[2];
  constexpr float kDcPole = 1.f - 2.f * 3.14159265f * 10.f / kOutRate;
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < 2; ++c) {
      float x = out[2 * i + c];
      float y = x - dc_x[c] + kDcPole * dc_y[c];
      dc_x[c] = x;
      dc_y[c] = y;
      out[2 * i + c] = y;
    }
  static std::vector<int16_t> pcm;
  pcm.resize(size_t(frames) * 2);
  for (int i = 0; i < frames; ++i) {
    float frame_peak = 0;
    for (int c = 0; c < 2; ++c) {
      float s = out[2 * i + c];
      float a = std::fabs(s);
      frame_peak = std::max(frame_peak, a);
      g_stats.sumsq += double(s) * s;
      g_win.sumsq += double(s) * s;
      if (a > 1.f) ++g_stats.clipped, ++g_win.clipped;  // would have clipped (counted before the limiter)
    }
    float& l = out[2 * i];
    float& r = out[2 * i + 1];
    float g = g_lim.run(l, r);
    if (g < 0.9886f) ++g_stats.reduced, ++g_win.reduced;  // -0.1 dB
    g_stats.min_gain = std::min<double>(g_stats.min_gain, g);
    g_win.min_gain = std::min<double>(g_win.min_gain, g);
    for (int c = 0; c < 2; ++c) {
      float s = out[2 * i + c] * g;
      if (std::fabs(s) > 1.f) {
        ++g_stats.post, ++g_win.post;
        s = std::clamp(s, -1.f, 1.f);
      }
      out[2 * i + c] = s;
      pcm[2 * i + c] = int16_t(std::lrint(s * 32767.f));
    }
    g_stats.peak = std::max<double>(g_stats.peak, frame_peak);
    g_win.peak = std::max<double>(g_win.peak, frame_peak);
    if (frame_peak > 1e-3f) ++g_stats.loud;
  }
  g_stats.frames += frames;
  g_win.frames += frames;
  if (g_wav) {
    fwrite(pcm.data(), 4, frames, g_wav);
    uint64_t before = g_wav_frames;
    g_wav_frames += frames;
    if (before / kOutRate != g_wav_frames / kOutRate) wav_header(g_wav, g_wav_frames);
  }
  static uint64_t next_log = 10ull * kOutRate;
  if (g_stats.frames >= next_log) {
    next_log += 10ull * kOutRate;
    log_window();
    log_stats("running");
  }
}

SDL_AudioStream* g_sdl_stream = nullptr;

void mixer_thread() {
  using clk = std::chrono::steady_clock;
  constexpr int kMaxQueued = kOutRate / 10;  // 100 ms in the device queue
  std::vector<float> buf;
  const auto start = clk::now();
  uint64_t done = 0;
  for (auto next = start;; ) {
    next += std::chrono::milliseconds(5);
    std::this_thread::sleep_until(next);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - start).count();
    uint64_t due = uint64_t(us) * kOutRate / 1000000;
    if (due <= done) continue;
    int frames = int(std::min<uint64_t>(due - done, kOutRate / 10));  // after a stall: catch up <= 100 ms
    done = due;
    buf.resize(size_t(frames) * 2);
    mix(buf.data(), frames);
    if (g_sdl_stream) {
      int queued = SDL_GetAudioStreamQueued(g_sdl_stream) / 8;
      if (queued + frames > kMaxQueued)  // safety net only (the controller keeps ~40 ms)
        g_dropped += frames;
      else
        SDL_PutAudioStreamData(g_sdl_stream, buf.data(), frames * 8);
      // Device clock drift: every 100 ms, nudge the device's playback ratio
      // so its queue stays near QueueController::kTarget (drift.h).
      static QueueController qc;
      static uint64_t last_ctl = 0;
      if (done - last_ctl >= kOutRate / 10) {
        double ratio = qc.update(double(queued) / kOutRate, double(done - last_ctl) / kOutRate);
        SDL_SetAudioStreamFrequencyRatio(g_sdl_stream, float(ratio));
        g_device_ratio = ratio;
        last_ctl = done;
      }
    }
  }
}

// ROTK_TEST_CLOCK=frames: exactly 800 frames (1/60 s at 48 kHz) per vblank,
// on the thread that advances the vblank (the game's Swap, or the kernel's
// idle vblank); completions are delivered there when it is a guest thread,
// else by the next DirectSoundDoWork. Positions, completions and the WAV are
// then functions of the frame number only.
void vblank_mix(uint64_t count, void*) {
  static uint64_t last = 0;
  static std::vector<float> buf;
  if (last == 0) {
    last = count - 1;
    rotk_log("dsound: test clock: mixer starts at vblank %llu (%s)", (unsigned long long)count,
            rotk_current_ctx() ? "guest thread" : "host thread");
  }
  static bool seen_guest = false;
  if (!seen_guest && rotk_current_ctx()) {
    seen_guest = true;
    rotk_log("dsound: test clock: first vblank on a guest thread: %llu", (unsigned long long)count);
  }
  int frames = int(std::min<uint64_t>(count - last, 60)) * int(kOutRate / 60);
  last = count;
  if (frames <= 0) return;
  buf.resize(size_t(frames) * 2);
  mix(buf.data(), frames);
  if (g_sdl_stream && SDL_GetAudioStreamQueued(g_sdl_stream) / 8 + frames <= int(kOutRate / 10))
    SDL_PutAudioStreamData(g_sdl_stream, buf.data(), frames * 8);
  if (RotkCtx* c = rotk_current_ctx()) deliver_completions(*c);
}

// SDL's audio subsystem is initialised here, from a static initialiser: on
// the process main thread, before main() and before any guest thread. SDL
// init is not safe to run concurrently with the window code's
// SDL_Init(VIDEO) on the main thread (rotk_host_main_loop), which is what a
// lazy init from the game thread at DirectSoundCreate would do. Opening the
// device stream later from the game thread is thread-safe in SDL3.
const bool g_sdl_audio_off = [] {
  const char* a = getenv("ROTK_AUDIO");
  return a && atoi(a) == 0;
}();
const bool g_sdl_audio = !g_sdl_audio_off && SDL_InitSubSystem(SDL_INIT_AUDIO);

void at_exit() {
  log_stats("exit");
  if (g_wav) wav_header(g_wav, g_wav_frames);
}

}  // namespace

void mixer_start() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (const char* p = getenv("ROTK_AUDIO_WAV"); p && *p) {
      g_wav = fopen(p, "wb");
      if (g_wav) {
        wav_header(g_wav, 0);
        fseek(g_wav, 44, SEEK_SET);
        rotk_log("dsound: writing the mix to %s", p);
      }
    }
    atexit(at_exit);
    if (g_sdl_audio) {
      SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kOutRate)};
      g_sdl_stream =
          SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      if (g_sdl_stream && SDL_ResumeAudioStreamDevice(g_sdl_stream)) {
        rotk_log("dsound: SDL audio on '%s' (48 kHz stereo, wall-clock mixer)",
                SDL_GetCurrentAudioDriver());
      } else {
        rotk_log("dsound: SDL audio unavailable (%s); wall-clock mixer without a device",
                SDL_GetError());
        g_sdl_stream = nullptr;
      }
    } else {
      rotk_log("dsound: no SDL audio (%s); wall-clock mixer without a device",
              g_sdl_audio_off ? "ROTK_AUDIO=0" : "SDL audio init failed");
    }
    if (test_clock_frames()) {
      rotk_vblank_add_callback(vblank_mix, nullptr);
      rotk_log("dsound: ROTK_TEST_CLOCK=frames: mixing 800 frames per vblank (no wall clock)");
    } else {
      std::thread(mixer_thread).detach();
    }
  });
}

}  // namespace rotkds
