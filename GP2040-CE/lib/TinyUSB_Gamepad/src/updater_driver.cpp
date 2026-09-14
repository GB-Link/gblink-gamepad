/*
 * SPDX-License-Identifier: MIT
 */

#include "updater_driver.h"

#include "tusb.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#define UPDATER_VID 0x2FE3
#define UPDATER_PID 0x000B
#define UPDATER_BCD_DEVICE \
    ((GBLINK_GAMEPAD_VERSION_MAJOR << 8) | (GBLINK_GAMEPAD_VERSION_MINOR << 4) | GBLINK_GAMEPAD_VERSION_PATCH)

#define EP_CMD_OUT   0x01
#define EP_CMD_IN    0x81
#define EP_DATA_OUT  0x02
#define EP_DATA_IN   0x82

#define VENDOR_CODE_WEBUSB 0x01
#define VENDOR_CODE_MSOS   0x02

#define CMD_GET_FIRMWARE_INFO 0x0F
#define CMD_REBOOT_BOOTLOADER 0x43
#define CMD_REBOOT            0x48

//--------------------------------------------------------------------
// Descriptors
//--------------------------------------------------------------------

static const uint8_t updater_device_descriptor[] = {
    18, TUSB_DESC_DEVICE,
    0x10, 0x02,             // bcdUSB 2.10 (BOS capable)
    0x00, 0x00, 0x00,       // class/subclass/protocol per interface
    64,                     // bMaxPacketSize0
    TU_U16_LOW(UPDATER_VID), TU_U16_HIGH(UPDATER_VID),
    TU_U16_LOW(UPDATER_PID), TU_U16_HIGH(UPDATER_PID),
    TU_U16_LOW(UPDATER_BCD_DEVICE), TU_U16_HIGH(UPDATER_BCD_DEVICE),
    0x01, 0x02, 0x03,       // iManufacturer, iProduct, iSerialNumber
    0x01,                   // bNumConfigurations
};

#define UPDATER_CONFIG_LEN (9 + 9 + 4 * 7)

static const uint8_t updater_configuration_descriptor[] = {
    9, TUSB_DESC_CONFIGURATION,
    TU_U16_LOW(UPDATER_CONFIG_LEN), TU_U16_HIGH(UPDATER_CONFIG_LEN),
    1,      // bNumInterfaces
    1,      // bConfigurationValue
    0,      // iConfiguration
    0x80,   // bus powered
    250,    // 500 mA (the link port rail runs off USB)

    9, TUSB_DESC_INTERFACE,
    0, 0, 4,            // interface 0, alt 0, 4 endpoints
    0xFF, 0x00, 0x00,   // vendor class
    0,

    7, TUSB_DESC_ENDPOINT, EP_CMD_OUT,  TUSB_XFER_BULK, 64, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_CMD_IN,   TUSB_XFER_BULK, 64, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_DATA_OUT, TUSB_XFER_BULK, 64, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_DATA_IN,  TUSB_XFER_BULK, 64, 0, 0,
};

// MS OS 2.0 descriptor set — single-interface device, so the WinUSB
// CompatibleID and the DeviceInterfaceGUIDs property live at set level.
#define MSOS2_SET_HEADER_LEN 10
#define MSOS2_COMPAT_ID_LEN  20
#define MSOS2_REG_PROP_LEN   (2 + 2 + 2 + 2 + 42 + 2 + 80)
#define MSOS2_TOTAL_LEN      (MSOS2_SET_HEADER_LEN + MSOS2_COMPAT_ID_LEN + MSOS2_REG_PROP_LEN)

#define U16C(c) (c), 0x00

static const uint8_t updater_msos2_descriptor[] = {
    TU_U16_LOW(MSOS2_SET_HEADER_LEN), TU_U16_HIGH(MSOS2_SET_HEADER_LEN),
    0x00, 0x00,             // MS_OS_20_SET_HEADER_DESCRIPTOR
    0x00, 0x00, 0x03, 0x06, // Windows 8.1+
    TU_U16_LOW(MSOS2_TOTAL_LEN), TU_U16_HIGH(MSOS2_TOTAL_LEN),

    TU_U16_LOW(MSOS2_COMPAT_ID_LEN), TU_U16_HIGH(MSOS2_COMPAT_ID_LEN),
    0x03, 0x00,             // MS_OS_20_FEATURE_COMPATBLE_ID
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    TU_U16_LOW(MSOS2_REG_PROP_LEN), TU_U16_HIGH(MSOS2_REG_PROP_LEN),
    0x04, 0x00,             // MS_OS_20_FEATURE_REG_PROPERTY
    0x07, 0x00,             // REG_MULTI_SZ
    42, 0x00,
    U16C('D'), U16C('e'), U16C('v'), U16C('i'), U16C('c'), U16C('e'),
    U16C('I'), U16C('n'), U16C('t'), U16C('e'), U16C('r'), U16C('f'),
    U16C('a'), U16C('c'), U16C('e'), U16C('G'), U16C('U'), U16C('I'),
    U16C('D'), U16C('s'), 0x00, 0x00,
    80, 0x00,
    U16C('{'), U16C('9'), U16C('D'), U16C('3'), U16C('2'), U16C('F'),
    U16C('8'), U16C('2'), U16C('C'), U16C('-'), U16C('1'), U16C('F'),
    U16C('B'), U16C('2'), U16C('-'), U16C('4'), U16C('4'), U16C('8'),
    U16C('6'), U16C('-'), U16C('8'), U16C('5'), U16C('0'), U16C('1'),
    U16C('-'), U16C('B'), U16C('6'), U16C('1'), U16C('4'), U16C('5'),
    U16C('B'), U16C('5'), U16C('B'), U16C('A'), U16C('3'), U16C('3'),
    U16C('6'), U16C('}'), 0x00, 0x00,
    0x00, 0x00,
};

static_assert(sizeof(updater_msos2_descriptor) == MSOS2_TOTAL_LEN, "MS OS 2.0 descriptor length mismatch");

#define BOS_TOTAL_LEN (5 + 24 + 28)

static const uint8_t updater_bos_descriptor[] = {
    5, TUSB_DESC_BOS,
    TU_U16_LOW(BOS_TOTAL_LEN), TU_U16_HIGH(BOS_TOTAL_LEN),
    2,

    // WebUSB platform capability (no landing page: iLandingPage 0)
    24, TUSB_DESC_DEVICE_CAPABILITY, DEVICE_CAPABILITY_PLATFORM, 0x00,
    0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
    0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65,
    0x00, 0x01, VENDOR_CODE_WEBUSB, 0x00,

    // Microsoft OS 2.0 platform capability
    28, TUSB_DESC_DEVICE_CAPABILITY, DEVICE_CAPABILITY_PLATFORM, 0x00,
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,
    TU_U16_LOW(MSOS2_TOTAL_LEN), TU_U16_HIGH(MSOS2_TOTAL_LEN),
    VENDOR_CODE_MSOS, 0x00,
};

static_assert(sizeof(updater_bos_descriptor) == BOS_TOTAL_LEN, "BOS descriptor length mismatch");

const uint8_t *updater_device_descriptor_cb(void) { return updater_device_descriptor; }
const uint8_t *updater_configuration_descriptor_cb(void) { return updater_configuration_descriptor; }
const uint8_t *updater_bos_descriptor_cb(void) { return updater_bos_descriptor; }

const uint16_t *updater_string_descriptor_cb(uint8_t index)
{
    static uint16_t desc[34];
    static const char *strings[] = { nullptr, "GB-Link", "GBLink Gamepad" };
    uint8_t len = 0;

    if (index == 0) {
        desc[1] = 0x0409; // English
        len = 1;
    } else if (index == 3) {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        for (; serial[len] && len < 32; len++) desc[1 + len] = serial[len];
    } else if (index < TU_ARRAY_SIZE(strings)) {
        for (; strings[index][len] && len < 32; len++) desc[1 + len] = strings[index][len];
    } else {
        return nullptr;
    }

    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc;
}

//--------------------------------------------------------------------
// Device-level vendor requests: MS OS 2.0 descriptor set
//--------------------------------------------------------------------

bool updater_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR
        && request->bRequest == VENDOR_CODE_MSOS && request->wIndex == 7) {
        return tud_control_xfer(rhport, request,
            (void *)(uintptr_t)updater_msos2_descriptor, sizeof(updater_msos2_descriptor));
    }
    return false;
}

//--------------------------------------------------------------------
// Class driver: bulk endpoints + command handling
//--------------------------------------------------------------------

static uint8_t cmd_out_buf[64];
static uint8_t data_out_buf[64];
static uint8_t reply_buf[64];

static void handle_command(uint8_t rhport, const uint8_t *cmd, uint32_t len)
{
    if (len == 0) return;

    switch (cmd[0]) {
        case CMD_GET_FIRMWARE_INFO:
            // Replies travel on the data IN endpoint, like GBLink-Firmware.
            reply_buf[0] = CMD_GET_FIRMWARE_INFO;
            reply_buf[1] = GBLINK_GAMEPAD_VERSION_MAJOR;
            reply_buf[2] = GBLINK_GAMEPAD_VERSION_MINOR;
            reply_buf[3] = GBLINK_GAMEPAD_VERSION_PATCH;
            if (!usbd_edpt_busy(rhport, EP_DATA_IN))
                usbd_edpt_xfer(rhport, EP_DATA_IN, reply_buf, 4);
            break;

        case CMD_REBOOT_BOOTLOADER:
            reset_usb_boot(0, 0);
            break;

        case CMD_REBOOT:
            watchdog_hw->scratch[5] = 0;
            watchdog_reboot(0, 0, 50);
            break;

        default:
            break;
    }
}

static void updater_init(void) {}

static void updater_reset(uint8_t rhport) { (void)rhport; }

static uint16_t updater_open(uint8_t rhport, tusb_desc_interface_t const *itf_descriptor, uint16_t max_length)
{
    if (itf_descriptor->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC) return 0;

    uint16_t driver_length = sizeof(tusb_desc_interface_t)
        + itf_descriptor->bNumEndpoints * sizeof(tusb_desc_endpoint_t);
    TU_VERIFY(max_length >= driver_length, 0);

    const uint8_t *p_desc = tu_desc_next((uint8_t const *)itf_descriptor);
    for (int i = 0; i < itf_descriptor->bNumEndpoints; i++) {
        TU_ASSERT(usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p_desc), 0);
        p_desc = tu_desc_next(p_desc);
    }

    usbd_edpt_xfer(rhport, EP_CMD_OUT, cmd_out_buf, sizeof(cmd_out_buf));
    usbd_edpt_xfer(rhport, EP_DATA_OUT, data_out_buf, sizeof(data_out_buf));

    return driver_length;
}

// Return false so TinyUSB answers GET/SET_INTERFACE itself; claiming them
// without completing the transfer hangs WebUSB's selectAlternateInterface().
static bool updater_control_xfer(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    (void)rhport; (void)stage; (void)request;
    return false;
}

static bool updater_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)result;

    if (ep_addr == EP_CMD_OUT) {
        handle_command(rhport, cmd_out_buf, xferred_bytes);
        usbd_edpt_xfer(rhport, EP_CMD_OUT, cmd_out_buf, sizeof(cmd_out_buf));
    } else if (ep_addr == EP_DATA_OUT) {
        // Nothing uses the data pipe in the updater; drain and re-arm.
        usbd_edpt_xfer(rhport, EP_DATA_OUT, data_out_buf, sizeof(data_out_buf));
    }
    return true;
}

const usbd_class_driver_t updater_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "UPDATER",
#endif
    .init = updater_init,
    .reset = updater_reset,
    .open = updater_open,
    .control_xfer_cb = updater_control_xfer,
    .xfer_cb = updater_xfer_cb,
    .sof = NULL,
};
