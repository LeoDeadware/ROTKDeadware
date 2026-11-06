#pragma once
// Master limiter (owner: Audio): look-ahead peak limiter, stereo-linked, in
// place of a clip or a soft knee. Why one is needed: the game pans every SFX in
// 5.1 (e.g. FL 0 dB + BL -3 dB) even with stereo audio flags, so the stereo
// fold-down sums front and rear of a side and dense scenes go over 0 dBFS,
// where the console's GP would saturate.
// Per frame the gain that keeps the frame at kThresh is held (min) over the
// next kAhead frames, released by a one-pole (kReleaseSec), then box-averaged
// over kAhead frames. An average over a window of held minima is never above
// the need of the frame it is applied to, so no frame leaves above kThresh;
// the gain moves smoothly (linear ramp over kAhead frames into a peak).
// Latency kAhead - 1 frames (1.3 ms at 48 kHz).
// No dependencies: src/hle/dsound/tests/limiter_test.cpp checks it.

#include <algorithm>
#include <cmath>
#include <iterator>

namespace rotkds {

class Limiter {
 public:
  static constexpr int kAhead = 64;
  static constexpr float kThresh = 0.98f;
  static constexpr float kReleaseSec = 0.08f;

  explicit Limiter(float rate) : release_(1.f - std::exp(-1.f / (kReleaseSec * rate))) {
    std::fill(std::begin(need_), std::end(need_), 1.f);
    std::fill(std::begin(held_), std::end(held_), 1.f);
  }

  // Takes a frame (l, r); replaces it with the frame kAhead - 1 back and
  // returns the gain to apply to that one.
  float run(float& l, float& r) {
    float a = std::max(std::fabs(l), std::fabs(r));
    std::swap(l, delay_[dpos_][0]);
    std::swap(r, delay_[dpos_][1]);
    dpos_ = (dpos_ + 1) % (kAhead - 1);
    need_[pos_] = a > kThresh ? kThresh / a : 1.f;
    float h = *std::min_element(std::begin(need_), std::end(need_));
    rel_ = std::min(h, rel_ + (1 - rel_) * release_);
    sum_ += rel_ - held_[pos_];
    held_[pos_] = rel_;
    pos_ = (pos_ + 1) % kAhead;
    return std::min(1.f, float(sum_ / kAhead));
  }

 private:
  float release_;
  float delay_[kAhead - 1][2] = {};
  float need_[kAhead];  // gain each of the last kAhead input frames needs
  float held_[kAhead];  // released minima, for the box average
  double sum_ = kAhead;
  float rel_ = 1;
  int pos_ = 0, dpos_ = 0;
};

}  // namespace rotkds
