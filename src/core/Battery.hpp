#pragma once
// ============================================================================
//  Battery.hpp – turn a Li-ion cell's voltage into a charge percentage
// ============================================================================
//
//  A lithium cell's voltage is NOT proportional to its charge. It drops quickly
//  from 4.2 V, then sits on a long flat stretch around 3.6–3.9 V, then falls off a
//  cliff below ~3.5 V. A straight line from 3.3 V to 4.2 V (what this code used
//  before) therefore reads too low in the middle and too high at the end: "30 %"
//  could be a few minutes from empty.
//
//  SOURCE of the table in Battery.cpp: the Meshtastic firmware's default LiPo
//  open-circuit-voltage (OCV) curve, OCV_ARRAY in src/power.h. Meshtastic's T-Deck
//  variant (variants/esp32s3/t-deck/variant.h) does not override it, so this is the
//  curve Meshtastic itself uses on the T-Deck. Checked 2026-11-18 at commit
//  6d41e279f1f51bd59f687b9d441c1bf47b1594fc:
//    https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/power.h
//  Only the eleven voltages were taken (measured facts), no code. Meshtastic is
//  GPL-3.0. We found no curve measured on the T-Deck's own battery, so this is a
//  generic LiPo curve, not a calibration.
//
//  Between the table's points we draw straight lines (piecewise-linear
//  interpolation), like numpy.interp().
//
//  OPEN-CIRCUIT means "at rest". Under load (backlight, Wi-Fi) the voltage sags, and
//  while charging it is pushed up, so the percentage reads a little low in use and
//  high on USB. The status bar's smoothing (hal/tdeck/Board.cpp) hides the jitter,
//  not this offset.
//
//  Pure arithmetic, no hardware, so the PC tests can check it.
// ============================================================================

namespace battery {

// 0..100 for a cell at `millivolts`. Clamped at both ends.
int percentFromMillivolts(int millivolts);

}  // namespace battery
