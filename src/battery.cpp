#include "battery.h"

// How the Cardputer ADV's power path shapes the readings (from its schematic):
// the battery voltage is read through a 1:2 divider on G10. When USB power is
// present, a power-path switch runs the whole device from USB and takes the
// battery off the load while the TP4057 charges it, so the reading jumps up by
// roughly 50-100 mV on plug-in and drops by the same on unplug. Neither the
// charger's status pins nor USB VBUS reach a GPIO, so these jumps are the only
// sign of plugging in or out.
//
// Strategy:
//  - convert voltage to percent with a Li-ion discharge curve (not a line),
//  - smooth samples with a moving average,
//  - on battery, only let the number go down (after a short settling window),
//  - detect plug-in by a sustained jump up: from then on estimate the real
//    charge by subtracting the size of that jump, and never show less than the
//    last honest on-battery value,
//  - detect unplugging by a sustained jump down and re-seed from the new
//    reading, since charging may have raised the true level.

namespace {

// Battery voltage under the device's own light load (~120 mA, about 0.07C for
// the 1750 mAh cell) vs. remaining charge. Typical Li-ion shape: steep near
// full, flat through the middle, falling off near empty. Brown-out is ~3.4 V.
struct Point { int mv; int pct; };
const Point kCurve[] = {
    {3450, 0},  {3600, 5},  {3660, 10}, {3710, 20}, {3740, 30}, {3770, 40},
    {3800, 50}, {3850, 60}, {3900, 70}, {3970, 80}, {4050, 90}, {4150, 100},
};
const int kCurveLen = sizeof(kCurve) / sizeof(kCurve[0]);

const int kJumpMv = 45;               // plug/unplug step, well above ADC noise
const int kJumpSamples = 2;           // consecutive samples needed to believe it
const uint32_t kSettleMs = 60000;     // free movement after boot / unplug
const int kSmoothing = 8;             // moving-average weight (~16 s at 2 s/sample)

// With the power switch off, the battery is disconnected from everything
// (including the charger), but USB still runs the device through a diode. The
// ADC then reads the charger's own output sitting at its ~4.2 V target with no
// battery on it, which used to show as a confident "100%". Treat a reading
// pegged up there as "no useful battery reading" and show nothing. A full
// battery on charge sits close to the same voltage and may also read as
// unknown, which is better than inventing a number.
const int kPeggedMv = 4190;
const int kPeggedClearMv = 4165;      // hysteresis
const uint32_t kPeggedMs = 20000;

bool s_have = false;
int s_avg = 0;            // smoothed voltage, mV
int s_display = -1;       // percent shown to the user
bool s_usb = false;
int s_usbOffset = 0;      // size of the plug-in jump, mV
int s_usbStartMv = 0;     // voltage when plug-in was detected
int s_stepDir = 0, s_stepCount = 0, s_stepSum = 0;  // pending out-of-range readings
uint32_t s_peggedSince = 0;
bool s_unknown = false;
uint32_t s_settleUntil = 0;

void reseed(uint32_t now, int mv) {
  s_avg = mv;
  s_display = batteryPercentForVoltage(mv);
  s_settleUntil = now + kSettleMs;
  s_stepDir = s_stepCount = 0;
  s_peggedSince = 0;
  s_unknown = false;
}

}  // namespace

int batteryPercentForVoltage(int mv) {
  if (mv <= kCurve[0].mv) return 0;
  if (mv >= kCurve[kCurveLen - 1].mv) return 100;
  for (int i = 1; i < kCurveLen; i++) {
    if (mv <= kCurve[i].mv) {
      const Point& a = kCurve[i - 1];
      const Point& b = kCurve[i];
      return a.pct + (mv - a.mv) * (b.pct - a.pct) / (b.mv - a.mv);
    }
  }
  return 100;
}

void batteryReset() {
  s_have = false;
  s_display = -1;
  s_usb = false;
  s_usbOffset = 0;
  s_stepDir = s_stepCount = 0;
  s_peggedSince = 0;
  s_unknown = false;
}

void batteryFeed(uint32_t now, int mv) {
  if (mv < 2500 || mv > 4600) return;  // nonsense reading (no battery, ADC glitch)
  if (!s_have) {
    s_have = true;
    reseed(now, mv);
    return;
  }

  // A reading far from the average is held back until the next one confirms
  // it: a lone blip (Wi-Fi burst, ADC glitch) is dropped instead of being
  // averaged in, where the only-go-down rule would make it permanent.
  int diff = mv - s_avg;
  if (diff > kJumpMv || diff < -kJumpMv) {
    int dir = diff > 0 ? 1 : -1;
    if (dir == s_stepDir) {
      s_stepCount++;
      s_stepSum += diff;
    } else {
      s_stepDir = dir;
      s_stepCount = 1;
      s_stepSum = diff;
    }
    if (s_stepCount < kJumpSamples) return;
    int step = s_stepSum / s_stepCount;
    s_stepDir = s_stepCount = 0;
    if (dir > 0 && !s_usb) {  // plugged in
      s_usb = true;
      s_usbOffset = step;
      s_usbStartMv = mv;
      s_avg = mv;
      return;
    }
    if (dir < 0 && s_usb) {  // unplugged: charging may have raised the true level
      s_usb = false;
      reseed(now, mv);
      return;
    }
    s_avg = mv;  // some other lasting change (e.g. a heavier load): follow it
  } else {
    s_stepDir = s_stepCount = 0;
    s_avg += (mv - s_avg) / kSmoothing;
  }

  // No battery on the reading (power switch off while plugged in)?
  if (s_avg >= kPeggedMv) {
    if (!s_peggedSince) s_peggedSince = now;
    if (!s_unknown && now - s_peggedSince >= kPeggedMs) {
      s_unknown = true;
      s_display = -1;
    }
  } else if (s_avg < kPeggedClearMv) {
    s_peggedSince = 0;
    if (s_unknown) {  // a battery is back on the reading
      s_unknown = false;
      reseed(now, s_avg);
    }
  }
  if (s_unknown) return;

  if (s_usb && s_avg < s_usbStartMv - 25) {
    // A charging battery never falls below its plug-in voltage: that "jump"
    // was something else (e.g. recovery after a heavy load). Back to battery.
    s_usb = false;
    reseed(now, s_avg);
    return;
  }

  if (s_usb) {
    // Charging: estimate the resting level by removing the charger's lift,
    // and never report less than the last on-battery value.
    int est = batteryPercentForVoltage(s_avg - s_usbOffset);
    if (est > s_display) s_display = est;
    return;
  }

  int pct = batteryPercentForVoltage(s_avg);
  if ((int32_t)(now - s_settleUntil) < 0) s_display = pct;  // still settling
  else if (pct < s_display) s_display = pct;                // on battery: only down
}

int batteryPercent() { return s_display; }
bool batteryOnUsb() { return s_usb; }
