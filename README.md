# gblink-gamepad

Turns a [GB-Link](https://gblink.io) adapter and a Game Boy Advance into a USB controller.

This is a fork of [gba-pico-gamepad](https://github.com/copyrat90/gba-pico-gamepad) by copyrat90, adapted to run on GB-Link hardware.

```
GBA  ── GBC link cable ──  GB-Link  ── USB ──  PC / Switch / PS3 / PS4
```


# Requirements

* GB-Link adapter
* Game Boy Advance, turned on **without a cartridge**
* **GBC** link cable (a GBA link cable will not work)


# Installing

Use the [GB-Link Launcher](https://launcher.gblink.io) in a Chromium-based browser:

1. Plug in the GB-Link with no GBA connected.
2. Open the device panel and install **GBLink Gamepad firmware**.

To go back to the regular GBLink firmware, do the same with the GBA off and pick **GBLink firmware**. The gamepad firmware only shows up in the launcher while no GBA is connected.

To flash manually, hold BOOTSEL while plugging in the GB-Link and copy `gblink-gamepad.uf2` to the `RPI-RP2` drive.


# Usage

1. Plug the GB-Link into your PC or console.
2. Connect the GBA with a GBC link cable and turn it on without a cartridge.\
   The GB-Link sends the controller program to the GBA, which takes about a second.
3. The GBA screen shows a mode menu for 5 seconds. Hold a button to pick a mode:

    | Hold | Mode |
    |---|---|
    | `A` | XInput (Xbox) |
    | `B` | Nintendo Switch |
    | `L` | DirectInput / PS3 |
    | `R` | PS4 |
    | *(nothing)* | Last used mode (XInput on first boot) |

    The last button held wins, and the choice is saved.
4. Play. The GBA screen shows the active mode.

To pick a different mode, turn the GBA off and on again. If the link is lost for about 2 seconds, the GB-Link restarts, waits for the GBA and shows the menu again.

* Face buttons follow the console's labels: in Switch mode GBA `B`/`A` are Switch `B`/`A`; in every other mode GBA `A`/`B` are Xbox `A`/`B` (PlayStation Cross/Circle).
* [D-pad modes](https://gp2040-ce.info/#/usage?id=d-pad-modes) can be changed with the usual GP2040-CE hotkeys.
* Holding `SELECT` + `START` + `UP` during the mode menu reboots the GB-Link into the USB bootloader.
* GP2040-CE's Web Config is disabled.

## Status LED

| LED | Meaning |
|---|---|
| Red | Waiting for the GBA, or link lost |
| Amber | No GBA yet; the launcher can reach the adapter |
| Blue | Sending the controller program to the GBA |
| Green | Connected |
| Magenta | Sending the program failed. Turn the GBA off, replug the GB-Link and try again |


# How it works

This is a modified [GP2040-CE](https://github.com/OpenStickCommunity/GP2040-CE) 0.7.1 that reads buttons from the GBA over the link port instead of from GPIO pins.

* The GBA runs a small program based on the `LinkSPI_demo` example from [gba-link-connection](https://github.com/rodri042/gba-link-connection). It sends its key state on every 32-bit link exchange and draws the mode menu from what the adapter sends back.
* The program is sent to the GBA with multiboot, ported from [gba_03_multiboot](https://github.com/akkera102/gba_03_multiboot) ([`GP2040-CE/src/gba/multiboot.cpp`](GP2040-CE/src/gba/multiboot.cpp)).
* The link runs on a PIO SPI master on the GB-Link's link port pins ([`GP2040-CE/src/gba/spi32.cpp`](GP2040-CE/src/gba/spi32.cpp)), using the same PIO programs as [GBLink-Firmware](https://github.com/GB-Link/GBLink-Firmware).
* While no GBA is connected, the adapter enumerates as a WebUSB "updater" device (VID `0x2FE3`, PID `0x000B`) that the launcher uses to read the firmware version and reboot into the bootloader ([`GP2040-CE/lib/TinyUSB_Gamepad/src/updater_driver.cpp`](GP2040-CE/lib/TinyUSB_Gamepad/src/updater_driver.cpp)).


# Build

Requirements:

* [devkitARM](https://devkitpro.org/wiki/Getting_Started) with the `gba-dev` package
* Pico SDK 1.5.0 and the Arm GNU toolchain 12.3, e.g. as installed by the Raspberry Pi Pico VS Code extension
* `cmake`, `make`, `python3`

```bash
./build.sh
```

The script defaults to `/opt/devkitpro`, `~/.pico-sdk/sdk/1.5.0` and `~/.pico-sdk/toolchain/12_3_Rel1`. Override them with `DEVKITPRO`, `PICO_SDK_PATH` and `PICO_TOOLCHAIN_PATH`.

The firmware is written to `build/gblink-gamepad.uf2`.

## Releasing

1. Bump the version in [`GP2040-CE/lib/TinyUSB_Gamepad/src/updater_driver.h`](GP2040-CE/lib/TinyUSB_Gamepad/src/updater_driver.h). The launcher reads it from the device.
2. Build, then copy the UF2 into the launcher's `assets/firmware/` as `gblink-gamepad.v<version>.uf2` and regenerate its manifest.


# Credits

* [gba-pico-gamepad](https://github.com/copyrat90/gba-pico-gamepad) : The original Raspberry Pi Pico project this fork is based on
* [gba-link-connection](https://github.com/rodri042/gba-link-connection) : Game Boy Advance (GBA) C++ libraries to interact with the Serial Port
* [GP2040-CE](https://github.com/OpenStickCommunity/GP2040-CE) : Gamepad firmware for the Raspberry Pi Pico
* [gba_03_multiboot](https://github.com/akkera102/gba_03_multiboot) : Raspberry Pi GBA Loader
* [GBLink-Firmware](https://github.com/GB-Link/GBLink-Firmware) : Link port PIO SPI and status LED programs


# License

See the license of each project above.\
`/build.sh` is 0BSD.
