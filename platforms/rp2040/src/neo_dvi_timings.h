#pragma once
// neo_dvi_timings.h — 800x480 modes of the Neo6502 builds at a higher system clock:
// same 800 active pixels, 480 lines, line and frame rates as dvi_timing_800x480p_60hz
// (295.2 MHz), with the horizontal blanking stretched. The TMDS bit clock is the
// system clock: more CPU time per emulated cycle (docs/STRATEGIE-NEO6502.md).
// Include after PicoDVI's dvi.h; in RAM, as PicoDVI's own timings.
//
// First written for the BBC Micro (platforms/rp2040/systems/bbc/src/bbc.c, ADR-04 of
// ~/Neo6502bbc): bmarty's screen accepts 330 and 345 MHz, not 372 (800x480_372).

// 345 MHz, blanking stretched to 1160 pixels per line; DVDD 1.25V
static const struct dvi_timing __not_in_flash_func(dvi_timing_800x480p_60hz_345) __attribute__((unused)) = {
    .h_sync_polarity = false,
    .h_front_porch = 164,
    .h_sync_width = 84,
    .h_back_porch = 112,
    .h_active_pixels = 800,
    .v_sync_polarity = true,
    .v_front_porch = 3,
    .v_sync_width = 10,
    .v_back_porch = 7,
    .v_active_lines = 480,
    .bit_clk_khz = 345000,
};

// 372 MHz; DVDD 1.3V: refused by bmarty's screen (or link)
static const struct dvi_timing __not_in_flash_func(dvi_timing_800x480p_60hz_372) __attribute__((unused)) = {
    .h_sync_polarity = false,
    .h_front_porch = 238,
    .h_sync_width = 92,
    .h_back_porch = 120,
    .h_active_pixels = 800,
    .v_sync_polarity = true,
    .v_front_porch = 3,
    .v_sync_width = 10,
    .v_back_porch = 7,
    .v_active_lines = 480,
    .bit_clk_khz = 372000,
};
