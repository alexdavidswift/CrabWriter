#pragma once
#include <stdint.h>

// Battery percentage estimate for a voltage-only battery gauge (the Cardputer
// ADV has no fuel gauge chip and no charge-status pin; see battery.cpp).
//
// Pure logic: feed it voltage samples, read back a steadied percentage. The
// caller does the ADC reads, which keeps this testable on a PC.

void batteryFeed(uint32_t nowMs, int millivolts);  // one sample, ~every 2 s
int batteryPercent();                             // -1 until the first sample
int batteryPercentForVoltage(int millivolts);     // the raw curve lookup
bool batteryOnUsb();                              // best guess, for diagnostics
void batteryReset();
