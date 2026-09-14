/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * GB-Link hardware port of the 32-bit GBA link exchange.
 *
 * Pinout (same as GBLink-Firmware; needs a GBC link cable):
 *   GP0  = SC   (clock -> GBA, idle high)
 *   GP1  = SIN  (GBA SO -> us)
 *   GP2  = SOUT (us -> GBA SI)
 *   GP11 = 3.3V rail enable (active low)
 *   GP12 = 5V rail enable (active low)
 *   GP16 = WS2812 status LED
 *
 * The RP2040's SPI peripheral cannot put SCK on GP0, so the SPI master is a
 * PIO state machine (~1 MHz, from GBLink-Firmware). Define
 * GBLINK_SPI_BITBANG=1 to use a ~250 kHz GPIO bit-bang instead.
 */

#include "gba/spi32.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#ifndef GBLINK_SPI_BITBANG
#define GBLINK_SPI_BITBANG 0
#endif

namespace gba
{

static constexpr uint PIN_SC   = 0;
static constexpr uint PIN_SIN  = 1;
static constexpr uint PIN_SOUT = 2;

static constexpr uint PIN_VSW_3V3 = 11;
static constexpr uint PIN_VSW_5V  = 12;
static constexpr uint PIN_WS2812  = 16;

// pio0 is used by the NeoPico LED addon
static PIO const GBLINK_PIO = pio1;
static constexpr uint LED_SM = 0;
static constexpr uint SPI_SM = 1;

// --- WS2812 status LED ---

static const uint16_t ws2812_program_instructions[] = {
    0x6221, //  0: out    x, 1       side 0 [2]
    0x1123, //  1: jmp    !x, 3      side 1 [1]
    0x1400, //  2: jmp    0          side 1 [4]
    0xa442, //  3: nop               side 0 [4]
};

static const struct pio_program ws2812_program = {
    .instructions = ws2812_program_instructions,
    .length = 4,
    .origin = -1,
};

static bool ledReady = false;

static void ledInit()
{
    pio_sm_claim(GBLINK_PIO, LED_SM);

    uint offset = pio_add_program(GBLINK_PIO, &ws2812_program);

    pio_gpio_init(GBLINK_PIO, PIN_WS2812);
    pio_sm_set_consecutive_pindirs(GBLINK_PIO, LED_SM, PIN_WS2812, 1, true);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, PIN_WS2812);
    sm_config_set_out_shift(&c, false, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_wrap(&c, offset, offset + 3);
    sm_config_set_clkdiv(&c, clock_get_hz(clk_sys) / (800000.0f * 8.0f));

    pio_sm_init(GBLINK_PIO, LED_SM, offset, &c);
    pio_sm_set_enabled(GBLINK_PIO, LED_SM, true);

    ledReady = true;
}

void setLed(uint8_t r, uint8_t g, uint8_t b)
{
    if (!ledReady) return;
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    if (!pio_sm_is_tx_fifo_full(GBLINK_PIO, LED_SM))
        pio_sm_put(GBLINK_PIO, LED_SM, grb << 8u);
}

// --- Link port SPI master ---

#if !GBLINK_SPI_BITBANG

// spi_cpha1 program from GBLink-Firmware's gbLinkLayer.c:
//   out x, 1    side 0     ; stall here on empty (SCK deasserted)
//   mov pins, x side 1 [1] ; output data, assert SCK
//   in pins, 1  side 0     ; input data, deassert SCK
static const uint16_t spi_program_instructions[] = {
    0x6021, //  out x, 1       side 0
    0xb101, //  mov pins, x    side 1 [1]
    0x4001, //  in pins, 1     side 0
};

static const struct pio_program spi_program = {
    .instructions = spi_program_instructions,
    .length = 3,
    .origin = -1,
};

static uint spiOffset = 0;

// Full pad and state machine configuration; safe to call repeatedly
static void configureLinkSM()
{
    pio_sm_set_enabled(GBLINK_PIO, SPI_SM, false);
    pio_sm_clear_fifos(GBLINK_PIO, SPI_SM);
    pio_sm_restart(GBLINK_PIO, SPI_SM);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_out_pins(&c, PIN_SOUT, 1);
    sm_config_set_in_pins(&c, PIN_SIN);
    sm_config_set_sideset_pins(&c, PIN_SC);
    sm_config_set_sideset(&c, 1, false, false);

    // MSB-first, autopush/autopull at 8 bits
    sm_config_set_out_shift(&c, false, true, 8);
    sm_config_set_in_shift(&c, false, true, 8);

    // ~985 kHz SCK, same as GBLink-Firmware
    sm_config_set_clkdiv(&c, 4058.838f / 128.0f);

    sm_config_set_wrap(&c, spiOffset, spiOffset + 2);

    pio_sm_set_pins_with_mask(GBLINK_PIO, SPI_SM, 0,
        (1u << PIN_SC) | (1u << PIN_SOUT));
    pio_sm_set_pindirs_with_mask(GBLINK_PIO, SPI_SM,
        (1u << PIN_SC) | (1u << PIN_SOUT),
        (1u << PIN_SC) | (1u << PIN_SOUT) | (1u << PIN_SIN));
    pio_sm_set_consecutive_pindirs(GBLINK_PIO, SPI_SM, PIN_SOUT, 1, true);

    pio_gpio_init(GBLINK_PIO, PIN_SOUT);
    pio_gpio_init(GBLINK_PIO, PIN_SIN);
    pio_gpio_init(GBLINK_PIO, PIN_SC);
    gpio_pull_up(PIN_SIN);

    // CPOL=1: invert SCK so the idle state is HIGH
    // (pio_gpio_init clears overrides, so this must follow it)
    gpio_set_outover(PIN_SC, GPIO_OVERRIDE_INVERT);

    // Bypass the input synchronizer for lower latency on SIN
    hw_set_bits(&GBLINK_PIO->input_sync_bypass, 1u << PIN_SIN);

    pio_sm_init(GBLINK_PIO, SPI_SM, spiOffset, &c);
    pio_sm_set_enabled(GBLINK_PIO, SPI_SM, true);
}

static void linkInit()
{
    pio_sm_claim(GBLINK_PIO, SPI_SM);
    spiOffset = pio_add_program(GBLINK_PIO, &spi_program);
    configureLinkSM();
}

static uint8_t spiByte(uint8_t tx)
{
    io_rw_8 *txfifo = (io_rw_8 *)&GBLINK_PIO->txf[SPI_SM];
    io_rw_8 *rxfifo = (io_rw_8 *)&GBLINK_PIO->rxf[SPI_SM];

    while (pio_sm_is_tx_fifo_full(GBLINK_PIO, SPI_SM)) tight_loop_contents();
    *txfifo = tx;
    while (pio_sm_is_rx_fifo_empty(GBLINK_PIO, SPI_SM)) tight_loop_contents();
    return *rxfifo;
}

uint32_t spi32(uint32_t val)
{
    uint32_t recv = 0;
    for (int shift = 24; shift >= 0; shift -= 8)
        recv = (recv << 8) | spiByte((val >> shift) & 0xFF);
    return recv;
}

#else // GBLINK_SPI_BITBANG

static constexpr uint32_t HALF_PERIOD_US = 2; // ~250 kHz SCK

static void linkInit()
{
    gpio_init(PIN_SC);
    gpio_set_dir(PIN_SC, GPIO_OUT);
    gpio_put(PIN_SC, 1); // idle high (mode 3)
    gpio_init(PIN_SOUT);
    gpio_set_dir(PIN_SOUT, GPIO_OUT);
    gpio_put(PIN_SOUT, 0);
    gpio_init(PIN_SIN);
    gpio_set_dir(PIN_SIN, GPIO_IN);
    gpio_pull_up(PIN_SIN);
}

uint32_t spi32(uint32_t val)
{
    uint32_t recv = 0;

    for (int i = 31; i >= 0; i--) {
        gpio_put(PIN_SC, 0);                    // falling edge: present data
        gpio_put(PIN_SOUT, (val >> i) & 1);
        busy_wait_us(HALF_PERIOD_US);
        gpio_put(PIN_SC, 1);                    // rising edge: both sides sample
        recv = (recv << 1) | (gpio_get(PIN_SIN) & 1);
        busy_wait_us(HALF_PERIOD_US);
    }

    return recv;
}

#endif // GBLINK_SPI_BITBANG

void initSpi32()
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    // Power the link port level shifters: both rails off, then 3.3V (GBA)
    gpio_init(PIN_VSW_3V3);
    gpio_set_dir(PIN_VSW_3V3, GPIO_OUT);
    gpio_put(PIN_VSW_3V3, 1);
    gpio_init(PIN_VSW_5V);
    gpio_set_dir(PIN_VSW_5V, GPIO_OUT);
    gpio_put(PIN_VSW_5V, 1);
    busy_wait_us(100);
    gpio_put(PIN_VSW_3V3, 0);

    linkInit();

    ledInit();
    setLed(0x08, 0x00, 0x00); // red: waiting for the GBA
}

void deinitSpi32()
{
    // The link stays idle between uses
}

uint32_t linkDiagSnapshot = 0;
volatile uint32_t modeScanWord = 0;
volatile uint32_t validFrames = 0;

void reinitLink()
{
    // Capture the pre-repair pin state (see spi32.h for the layout)
    linkDiagSnapshot = ((iobank0_hw->io[PIN_SC].ctrl & 0xFFFFu) << 16)
        | ((padsbank0_hw->io[PIN_SC] & 0xFFu) << 8)
        | (padsbank0_hw->io[PIN_SOUT] & 0xFFu);

    // Re-assert the 3.3V rail selection without glitching it
    gpio_put(PIN_VSW_3V3, 0);
    gpio_put(PIN_VSW_5V, 1);
    gpio_set_dir(PIN_VSW_3V3, GPIO_OUT);
    gpio_set_dir(PIN_VSW_5V, GPIO_OUT);
    gpio_set_function(PIN_VSW_3V3, GPIO_FUNC_SIO);
    gpio_set_function(PIN_VSW_5V, GPIO_FUNC_SIO);

#if !GBLINK_SPI_BITBANG
    configureLinkSM();
#else
    linkInit();
#endif
}

}
