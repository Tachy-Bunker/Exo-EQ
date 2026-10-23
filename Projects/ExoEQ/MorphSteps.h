#pragma once

// Discrete position count for the per-band Morph knob, by filter-type ordinal (shared).
// 0 = continuous. Moved out of ExoEQ.cpp for T3-3 so the circle view can read it.
inline int MorphStepsForType(int ordinal)
{
  switch (ordinal)
  {
    case 14: case 15: case 20: case 21: case 22: case 23: case 24: return 4; // AW LP2/HP2, X family, Holt: 1-4 stages
    case 17: case 18: case 19: return 2;                                    // AW Z-HP/BP/Notch: 1 or 2 stages (threshold)
    case 35: return 4;                                                      // AW SlewSonic: wet = morph*3 -> stage levels 0-3
    case 36: case 37: return 2;                                             // Comb FF/FB: polarity (threshold)
    case 41: return 8;                                                      // Phaser: 1-8 stages
    case 42: case 43: return 8;                                             // Phys Odd/Even: 1-8 partials
    default: return 0;                                                      // continuous (Z-Lowpass crossfade, Y/BQNL/AverMatrix wet, etc.)
  }
}
