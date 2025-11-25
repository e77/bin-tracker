#pragma once
#include <stdint.h>
#include <pgmspace.h>

// Minimal placeholder truck bitmap so the sketch can compile even if the
// original asset is unavailable. Replace with your own RGB565 image data when
// ready.
static const int garbageTruckWidth = 1;
static const int garbageTruckHeight = 1;
static const uint16_t garbageTruckBitmap[garbageTruckWidth * garbageTruckHeight] PROGMEM = {
  0xFFFF
};
