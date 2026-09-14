/*
 * SPDX-License-Identifier: MIT
 *
 * "Updater" USB identity for the GB-Link gamepad firmware. Enumerated while
 * no GBA is connected so the GBLink launcher can reach the adapter and reboot
 * it into the RP2040 bootloader (BOOTSEL) for reflashing — no button press.
 *
 * Same vendor-interface framing as GBLink-Firmware (VID 0x2FE3):
 *   EP1 OUT = commands, EP2 IN = replies, EP2 OUT = data (unused)
 *   0x0F GetFirmwareInfo -> [0x0F, major, minor, patch]
 *   0x43 RebootBootloader (reset_usb_boot; does not return)
 *   0x48 Reboot (warm reboot back into this firmware)
 * PID 0x000B tells the launcher this is the gamepad firmware (main = 0x000A).
 */

#pragma once

#include <stdint.h>
#include "device/usbd_pvt.h"

#define GBLINK_GAMEPAD_VERSION_MAJOR 1
#define GBLINK_GAMEPAD_VERSION_MINOR 0
#define GBLINK_GAMEPAD_VERSION_PATCH 0

extern const usbd_class_driver_t updater_driver;

const uint8_t *updater_device_descriptor_cb(void);
const uint8_t *updater_configuration_descriptor_cb(void);
const uint8_t *updater_bos_descriptor_cb(void);
const uint16_t *updater_string_descriptor_cb(uint8_t index);
bool updater_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request);
