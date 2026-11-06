// Host test: the master limiter (limiter.h) never lets a frame out above its
// threshold, leaves material under it untouched (after the delay), and its gain
// recovers after a peak.
//
//   tests/limiter_test.sh
//
// Signals: a quiet sine (must pass bit-exact, delayed); random bursts of
// summed sines and single-sample spikes up to +12 dBFS (none out above
// kThresh); a step down after a loud passage (gain back near 1 within ~0.5 s).

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "hle/dsound/limiter.h"

using rotkds::Limiter;

int main() {
  const float rate = 48000;
  const int d = Limiter::kAhead - 1;
  int fails = 0;

  {  // quiet: identity after the delay
    Limiter lim(rate);
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.9f * std::sin(i * 0.05f);
    int bad = 0;
    for (size_t i = 0; i < x.size(); ++i) {
      float l = x[i], r = -x[i];
      float g = lim.run(l, r);
      float want = i >= size_t(d) ? x[i - d] : 0;
      if (g != 1.f || l != want || r != -want) ++bad;
    }
    printf("quiet sine 0.9: %d frames changed\n", bad);
    fails += bad != 0;
  }

  {  // loud: random bursts and spikes, nothing out above the threshold
    Limiter lim(rate);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(0, 1);
    long over = 0, frames = 0;
    float worst = 0, min_g = 1;
    for (int burst = 0; burst < 2000; ++burst) {
      int len = 1 + int(u(rng) * 4000);
      float amp = 4 * u(rng), f1 = 0.3f * u(rng), f2 = 0.3f * u(rng);
      bool spike = u(rng) < 0.2f;
      for (int i = 0; i < len; ++i, ++frames) {
        float l = spike ? (i == 0 ? amp : 0) : amp * (0.6f * std::sin(i * f1) + 0.4f * std::sin(i * f2));
        float r = spike ? 0 : amp * std::sin(i * f2);
        float g = lim.run(l, r);
        min_g = std::min(min_g, g);
        float m = std::max(std::fabs(l * g), std::fabs(r * g));
        worst = std::max(worst, m);
        if (m > Limiter::kThresh * 1.0001f) ++over;
      }
    }
    printf("bursts up to +12 dBFS: %ld frames, %ld out above %.2f (max out %.4f, min gain %.1f dB)\n", frames,
           over, Limiter::kThresh, worst, 20 * std::log10(min_g));
    fails += over != 0;
  }

  {  // recovery: 1 s at 2.0, then 0.5; gain back above -0.1 dB within 0.5 s
    Limiter lim(rate);
    int back = -1;
    for (int i = 0; i < 96000; ++i) {
      float a = i < 48000 ? 2.f : 0.5f;
      float l = a * std::sin(i * 0.05f), r = l;
      float g = lim.run(l, r);
      if (i >= 48000 + d && back < 0 && g > 0.9886f) back = i - 48000 - d;
    }
    printf("recovery after -6 dB of reduction: %.0f ms\n", back < 0 ? -1.0 : back * 1000.0 / rate);
    fails += back < 0 || back > rate / 2;
  }

  printf(fails ? "limiter: FAIL\n" : "limiter: PASS\n");
  return fails ? 1 : 0;
}
