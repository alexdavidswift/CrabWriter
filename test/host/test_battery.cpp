// Simulated battery traces for the percentage estimator (src/battery.cpp).
// Included from test_main.cpp, which provides CHECK.

#include <cmath>
#include <functional>
#include "battery.h"

namespace {

const uint32_t kStep = 2000;  // ms between samples, as on the device

// Runs `seconds` of samples. restMv(t) is the battery's true (on-battery,
// under-load) voltage; readingMv(t) is what the ADC sees. Calls check(t, shown)
// after every sample.
struct Sim {
  uint32_t now = 10000;
  std::mt19937 rng{42};
  int noise = 15;  // +/- mV
  void run(int seconds, const std::function<int(uint32_t)>& readingMv,
           const std::function<void(uint32_t, int)>& check = nullptr) {
    for (int t = 0; t < seconds * 1000; t += kStep) {
      int n = noise ? (int)(rng() % (2 * noise + 1)) - noise : 0;
      batteryFeed(now, readingMv(t) + n);
      if (check) check(t, batteryPercent());
      now += kStep;
    }
  }
};

}  // namespace

static void testBattery() {
  printf("battery estimator\n");

  // --- the curve itself
  CHECK(batteryPercentForVoltage(3000) == 0);
  CHECK(batteryPercentForVoltage(4300) == 100);
  int prev = -1;
  bool monotonic = true;
  for (int mv = 3400; mv <= 4200; mv += 5) {
    int p = batteryPercentForVoltage(mv);
    if (p < prev) monotonic = false;
    prev = p;
  }
  CHECK(monotonic);

  // --- steady discharge: 4.10 V -> 3.60 V over 6 hours, noisy ADC
  {
    batteryReset();
    Sim sim;
    const int secs = 6 * 3600;
    auto rest = [&](uint32_t t) { return 4100 - (int)(500.0 * t / (secs * 1000.0)); };
    int last = 101, maxErr = 0, rises = 0;
    sim.run(secs, rest, [&](uint32_t t, int shown) {
      if (t > 120000) {
        if (shown > last) rises++;
        maxErr = std::max(maxErr, std::abs(shown - batteryPercentForVoltage(rest(t))));
      }
      last = shown;
    });
    CHECK(rises == 0);   // never ticks back up while discharging
    CHECK(maxErr <= 3);  // tracks the true level closely
    CHECK(!batteryOnUsb());
  }

  // --- plug in mid-discharge, charge for an hour, unplug
  {
    batteryReset();
    Sim sim;
    const int lift = 75;  // charger lift seen on the device (~9%)
    sim.run(300, [](uint32_t) { return 3850; });  // 5 min on battery at ~60%
    int before = batteryPercent();
    CHECK(std::abs(before - 60) <= 2);

    // Plugged in: true level climbs 3.85 -> 3.97 V over an hour.
    auto restCharging = [](uint32_t t) { return 3850 + (int)(120.0 * t / 3600000.0); };
    int firstMinuteMax = 0, lastShown = 0, drops = 0;
    sim.run(3600, [&](uint32_t t) { return restCharging(t) + lift; }, [&](uint32_t t, int shown) {
      if (t < 60000) firstMinuteMax = std::max(firstMinuteMax, shown);
      if (shown < lastShown) drops++;
      lastShown = shown;
    });
    CHECK(batteryOnUsb());
    CHECK(firstMinuteMax <= before + 2);  // no 9% jump on plug-in
    CHECK(drops == 0);                    // never goes down while charging
    int endTrue = batteryPercentForVoltage(restCharging(3600000));
    CHECK(std::abs(lastShown - endTrue) <= 5);  // estimate kept up with charging

    // Unplug: reading drops by the lift; shows the true (higher) level quickly.
    int restNow = restCharging(3600000);
    int shownAfter30s = -1;
    sim.run(120, [&](uint32_t) { return restNow; }, [&](uint32_t t, int shown) {
      if (t == 30000) shownAfter30s = shown;
    });
    CHECK(!batteryOnUsb());
    CHECK(std::abs(shownAfter30s - batteryPercentForVoltage(restNow)) <= 3);
    CHECK(shownAfter30s > before);  // charging really raised it
  }

  // --- one-sample blips (Wi-Fi bursts, ADC glitches) change nothing
  {
    batteryReset();
    Sim sim;
    sim.run(120, [](uint32_t) { return 3800; });
    int base = batteryPercent();
    int maxDev = 0;
    sim.run(1800, [](uint32_t t) {
      uint32_t k = t / kStep;
      if (k % 17 == 5) return 3800 - 90;  // brief dip
      if (k % 23 == 11) return 3800 + 60; // brief spike
      return 3800;
    }, [&](uint32_t, int shown) { maxDev = std::max(maxDev, std::abs(shown - base)); });
    CHECK(!batteryOnUsb());
    CHECK(maxDev <= 2);
  }

  // --- a sustained rise that isn't a charger (load removed) must not stick
  {
    batteryReset();
    Sim sim;
    sim.run(300, [](uint32_t) { return 3800; });
    sim.run(10, [](uint32_t) { return 3860; });  // looks like a plug-in...
    // ...then keeps discharging from where it really was
    sim.run(1800, [](uint32_t t) { return 3800 - (int)(t / 60000); });
    CHECK(!batteryOnUsb());
    CHECK(std::abs(batteryPercent() - batteryPercentForVoltage(3770)) <= 4);
  }

  // --- booted while already on a charger (no jump to see), then unplugged
  {
    batteryReset();
    Sim sim;
    sim.run(600, [](uint32_t) { return 3930 + 75; });  // charging, inflated
    int inflated = batteryPercent();
    sim.run(300, [](uint32_t) { return 3930; });       // unplugged
    CHECK(batteryPercent() < inflated);
    CHECK(std::abs(batteryPercent() - batteryPercentForVoltage(3930)) <= 3);
  }

  // --- garbage readings are ignored
  {
    batteryReset();
    batteryFeed(1000, 0);
    batteryFeed(3000, 9999);
    CHECK(batteryPercent() == -1);
  }
}
