/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2021 Jason Skuby (mytechtoybox.com)
 */

#pragma once

#include "gamepad/GamepadDescriptors.h"

typedef enum
{
	USB_MODE_HID,
	USB_MODE_NET,
	USB_MODE_UPDATER, // WebUSB updater identity while no GBA is connected
} UsbMode;

InputMode get_input_mode(void);
bool get_usb_mounted(void);
void initialize_driver(InputMode mode);
// TinyUSB cannot switch identities once started; reboot before initialize_driver()
void initialize_updater(void);
void receive_report(uint8_t *buffer);
void send_report(void *report, uint16_t report_size);

