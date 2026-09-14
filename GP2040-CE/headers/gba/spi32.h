/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * GB-Link hardware port of the 32-bit GBA link exchange. Requires a GBC link
 * cable to the GBA.
 */

#pragma once

#include <cstdint>

namespace gba
{

void initSpi32();
void deinitSpi32();
uint32_t spi32(uint32_t val);

// Rebuild the link pad and PIO configuration from scratch. The pin state
// found beforehand is saved in linkDiagSnapshot:
//   bits 31:16 = IO_BANK0 GPIO0 ctrl (expect funcsel 7 = PIO1, outover 01 = invert)
//   bits 15:8  = PADS GPIO0
//   bits  7:0  = PADS GPIO2
void reinitLink();
extern uint32_t linkDiagSnapshot;

// Adapter -> GBA words, decoded by the controller ROM for its screen:
//   0xB007MMSS  mode-select window: MM = selected mode, SS = seconds left
//   0xC0MMbbbb  running: MM = active mode, bbbb = button echo
//   anything else: diagnostic pin snapshot
// Modes are InputMode values. GBA -> adapter words are 0xA5A5 | keys.
extern volatile uint32_t modeScanWord; // nonzero only during the boot scan
extern volatile uint32_t validFrames;  // count of valid GBA frames received
inline uint32_t modeScanFrame(uint8_t mode, uint8_t secondsLeft)
{
    return 0xB0070000u | ((uint32_t)mode << 8) | secondsLeft;
}
inline uint32_t runningFrame(uint8_t mode, uint32_t buttons)
{
    return 0xC0000000u | ((uint32_t)mode << 16) | (buttons & 0xFFFFu);
}

// GB-Link status LED (WS2812 on GP16): red = no GBA link, amber = updater
// on USB, blue = sending ROM, magenta = multiboot failed, green = link up
void setLed(uint8_t r, uint8_t g, uint8_t b);

}
