#include <tonc.h>
#include "../../_lib/interrupt.h"

// (0) Include the header
#include "../../../lib/LinkSPI.h"

void log(const char* text);
void wait(u32 verticalLines);
inline void VBLANK() {}

// (1) Create a LinkSPI instance (static, to avoid the heap)
static LinkSPI linkSPIInstance;
LinkSPI* linkSPI = &linkSPIInstance;

void init() {
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
  tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));

  // (2) Add the interrupt service routines
  interrupt_init();
  interrupt_set_handler(INTR_VBLANK, VBLANK);
  interrupt_enable(INTR_VBLANK);
  interrupt_set_handler(INTR_SERIAL, LINK_SPI_ISR_SERIAL);
  interrupt_enable(INTR_SERIAL);
}

// Allocation-free text helpers. Lines are padded to a fixed width so the
// screen can be redrawn in place without erasing (no flicker).

static const int LINE_WIDTH = 29; // 30 columns; keep one spare

static void appendStr(char*& p, const char* s) {
  while (*s) *p++ = *s++;
}

static void appendDec(char*& p, u32 v, int width) {
  char tmp[10];
  int i = 0;
  do {
    tmp[i++] = '0' + (v % 10);
    v /= 10;
  } while (v && i < 10);
  for (int pad = width - i; pad > 0; pad--) *p++ = ' ';
  while (i > 0) *p++ = tmp[--i];
}

static void appendHex(char*& p, u32 v) {
  for (int shift = 28; shift >= 0; shift -= 4)
    *p++ = "0123456789abcdef"[(v >> shift) & 0xF];
}

// Append one screen line, padded to LINE_WIDTH
static void line(char*& p, const char* text) {
  int n = 0;
  while (*text && n < LINE_WIDTH) { *p++ = *text++; n++; }
  while (n++ < LINE_WIDTH) *p++ = ' ';
  *p++ = '\n';
}

static const char* modeName(u32 mode) {
  switch (mode) {
    case 0: return "XInput (Xbox)";
    case 1: return "Switch";
    case 2: return "DInput / PS3";
    case 3: return "Keyboard";
    case 4: return "PS4";
    default: return "?";
  }
}

int main() {
  init();

  // alive = loop iterations, n = completed exchanges, t = timed-out exchanges
  u32 alive = 0, n = 0, t = 0, lastRecv = 0;
  bool timedOut = true;
  static char buf[512];

  tte_erase_screen();

  linkSPI->activate(LinkSPI::Mode::SLAVE);

  while (true) {
    alive++;
    u16 keys = ~REG_KEYS & KEY_ANY;

    // Redraw only every few exchanges so the link stays armed most of the time
    if (timedOut || (alive & 7) == 0) {
      char* p = buf;
      char tmp[40];
      char* q;

      line(p, "[gba-pico-gamepad]");
      line(p, "");

      if ((lastRecv >> 16) == 0xB007) {
        // Mode-select window: 0xB007MMSS
        u32 mode = (lastRecv >> 8) & 0xFF;
        u32 secs = lastRecv & 0xFF;

        q = tmp; appendStr(q, "MODE: "); appendStr(q, modeName(mode)); *q = 0;
        line(p, tmp);
        line(p, "");
        line(p, "HOLD A BUTTON TO PICK:");
        line(p, "  A   XInput (Xbox)");
        line(p, "  B   Switch");
        line(p, "  L   DInput / PS3");
        line(p, "  R   PS4");
        line(p, "");
        q = tmp; appendStr(q, "Starting in "); appendDec(q, secs, 1); appendStr(q, "..."); *q = 0;
        line(p, tmp);
      } else if ((lastRecv >> 24) == 0xC0) {
        // Running: 0xC0MMbbbb
        u32 mode = (lastRecv >> 16) & 0xFF;

        q = tmp; appendStr(q, "MODE: "); appendStr(q, modeName(mode)); *q = 0;
        line(p, tmp);
        line(p, "");
        line(p, "Connected. Have fun!");
        line(p, "");
        line(p, "Power cycle the GBA to");
        line(p, "pick a different mode.");
        line(p, "");
        line(p, "");
        line(p, "");
      } else {
        // Anything else: link diagnostics from the adapter
        line(p, "MODE: (waiting for adapter)");
        line(p, "");
        line(p, "");
        line(p, "");
        line(p, "");
        line(p, "");
        line(p, "");
        line(p, "");
        line(p, "");
      }

      line(p, "");
      line(p, "");
      q = tmp;
      appendStr(q, "link n:"); appendDec(q, n, 8);
      appendStr(q, "  t:"); appendDec(q, t, 6); *q = 0;
      line(p, tmp);
      q = tmp;
      appendStr(q, "keys:"); appendDec(q, keys, 4);
      appendStr(q, "  recv:"); appendHex(q, lastRecv); *q = 0;
      line(p, tmp);
      q = tmp;
      appendStr(q, "alive:"); appendDec(q, alive, 8); *q = 0;
      line(p, tmp);

      *p = '\0';
      log(buf);
    }

    // Exchange 32-bit data with the other end, timing out after ~100ms so the
    // screen keeps updating without a link partner. The 0xA5A5 marker lets
    // the adapter tell a live frame from an idle bus.
    u32 timeoutTicks = 0;
    u32 recv = linkSPI->transfer(
        0xA5A50000 | keys,
        [&timeoutTicks]() { return ++timeoutTicks > 200000; });
    timedOut = timeoutTicks > 200000;
    if (timedOut) {
      t++;
    } else {
      n++;
      lastRecv = recv;
    }
  }

  return 0;
}

void log(const char* text) {
  tte_write("#{P:0,0}");
  tte_write(text);
}

void wait(u32 verticalLines) {
  u32 count = 0;
  u32 vCount = REG_VCOUNT;

  while (count < verticalLines) {
    if (REG_VCOUNT != vCount) {
      count++;
      vCount = REG_VCOUNT;
    }
  };
}
