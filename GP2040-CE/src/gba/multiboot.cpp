/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * This is a port of `gba_03_multiboot` to the RPi Pico.
 * https://github.com/akkera102/gba_03_multiboot
 */

#include "gba/multiboot.h"

#include <cstdlib>
#include "pico/stdlib.h"

#include "gba/spi32.h"
#include "gba/GBAKey.h"
#include "usb_driver.h"

#include "tusb.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

// How long to wait for a GBA before bringing up the updater USB identity
#ifndef GBA_UPDATER_DELAY_MS
#define GBA_UPDATER_DELAY_MS 1500
#endif

namespace gba
{

// Delay after each handshake/control word
static constexpr int GBA_DELAY_MS = 3;

// Delay after each header/data word. Raise it if transfers fail (magenta LED).
#ifndef GBA_DATA_DELAY_US
#define GBA_DATA_DELAY_US 100
#endif

uint32_t spi32Delay(uint32_t val) {
    uint32_t result = gba::spi32(val);
    sleep_ms(GBA_DELAY_MS);

    return result;
}

static uint32_t spi32Fast(uint32_t val) {
    uint32_t result = gba::spi32(val);
    busy_wait_us(GBA_DATA_DELAY_US);

    return result;
}

bool sendGBARom(const uint8_t* romAddr, const uint32_t romSize) {
    initSpi32();

    uint32_t recv;
    uint32_t fsize = romSize;

    // -----------------------------------------------------
    // printf("Waiting for GBA...\n");
    
    // Wait for the BIOS multiboot handshake (0x7202) or a frame from an
    // already-running controller ROM (0xA5A5).
    //
    // If no GBA shows up, enumerate as the WebUSB updater so the GBLink
    // launcher can reach the adapter. TinyUSB cannot change identity once
    // started, so when a GBA then appears, reboot and start over.
    uint32_t waitedMs = 0;
    bool updaterUp = false;
    do {
        recv = gba::spi32(0x6202);
        if (updaterUp) {
            for (int i = 0; i < 10; i++) {
                tud_task();
                sleep_ms(1);
            }
        } else {
            sleep_ms(10);
        }
        waitedMs += 10;
        if (!updaterUp && waitedMs >= GBA_UPDATER_DELAY_MS) {
            initialize_updater();
            updaterUp = true;
            setLed(0x10, 0x04, 0x00); // amber: updater on USB
        }
    } while ((recv >> 16) != 0x7202 && (recv >> 16) != 0xA5A5);

    if (updaterUp) {
        watchdog_hw->scratch[5] = 0; // System::BootMode::DEFAULT
        watchdog_reboot(0, 0, 50);
        while (true) { __wfi(); }
    }

    if ((recv >> 16) == 0xA5A5)
        return false;

    setLed(0x00, 0x00, 0x10); // blue: sending the ROM

    // -----------------------------------------------------
    // printf("Sending header.\n");

    spi32Delay(0x6102);

    const uint16_t* fdata16 = (const uint16_t*)romAddr;
    for (uint32_t i = 0; i < 0xC0; i += 2)
        spi32Fast(fdata16[i / 2]);

    spi32Delay(0x6200);

    // -----------------------------------------------------
    // printf("Getting encryption and crc seeds.\n");

    spi32Delay(0x6202);
    spi32Delay(0x63D1);

    uint32_t token = spi32Delay(0x63D1);

    if ((token >> 24) != 0x73)
    {
        // fprintf(stderr, "Failed handshake!\n");
        setLed(0x10, 0x00, 0x10); // magenta: handshake failed
        exit(1);
    }


    uint32_t crcA, crcB, crcC, seed;

    crcA = (token >> 16) & 0xFF;
    seed = 0xFFFF00D1 | (crcA << 8);
    crcA = (crcA + 0xF) & 0xFF;

    spi32Delay(0x6400 | crcA);

    fsize += 0xF;
    fsize &= ~0xF;

    token = spi32Delay((fsize - 0x190) / 4);
    crcB = (token >> 16) & 0xFF;
    crcC = 0xC387;

    // -----------------------------------------------------
    // printf("Sending...\n");
    
    const uint32_t* fdata32 = (const uint32_t*)romAddr;

    for (uint32_t i = 0xC0; i < fsize; i += 4)
    {
        uint32_t dat = fdata32[i / 4];

        // crc step
        uint32_t tmp = dat;

        for (uint32_t b = 0; b < 32; b++)
        {
            uint32_t bit = (crcC ^ tmp) & 1;

            crcC = (crcC >> 1) ^ (bit ? 0xc37b : 0);
            tmp >>= 1;
        }

        // encrypt step
        seed = seed * 0x6F646573 + 1;
        dat = seed ^ dat ^ (0xFE000000 - i) ^ 0x43202F2F;

        // send
        uint32_t chk = spi32Fast(dat) >> 16;

        if (chk != (i & 0xFFFF))
        {
            // fprintf(stderr, "Transmission error at byte %zu: chk == %08x\n", i, chk);
            setLed(0x10, 0x00, 0x10); // magenta: transfer failed
            exit(1);
        }
    }

    // crc step final
    uint32_t tmp = 0xFFFF0000 | (crcB << 8) | crcA;

    for (uint32_t b = 0; b < 32; b++)
    {
        uint32_t bit = (crcC ^ tmp) & 1;

        crcC = (crcC >> 1) ^ (bit ? 0xc37b : 0);
        tmp >>= 1;
    }

    // -----------------------------------------------------
    // printf("Waiting for checksum...\n");

    spi32Delay(0x0065);

    do
    {
        recv = spi32Delay(0x0065) >> 16;
        sleep_us(10000);

    } while (recv != 0x0075);

    spi32Delay(0x0066);
    uint32_t crcGBA = spi32Delay(crcC & 0xFFFF) >> 16;

    // printf("Gba: %x, Cal: %x\n", crcGBA, crcC);
    // printf("Done.\n");

    deinitSpi32();

    return true;
}

}
