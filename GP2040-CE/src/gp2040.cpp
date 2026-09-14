// GP2040 includes
#include "gp2040.h"
#include "helper.h"
#include "system.h"
#include "gba/spi32.h"

#include "configmanager.h" // Global Managers
#include "storagemanager.h"
#include "addonmanager.h"

#include "addons/analog.h" // Inputs for Core0
#include "addons/bootsel_button.h"
#include "addons/dualdirectional.h"
#include "addons/extra_button.h"
#include "addons/i2canalog1219.h"
#include "addons/jslider.h"
#include "addons/playernum.h"
#include "addons/reverse.h"
#include "addons/turbo.h"
#include "addons/slider_socd.h"
#include "addons/wiiext.h"

// Pico includes
#include "pico/bootrom.h"
#include "pico/time.h"
#include "hardware/adc.h"

// TinyUSB
#include "usb_driver.h"
#include "tusb.h"

#define GAMEPAD_DEBOUNCE_MILLIS 5 // make this a class object

static const uint32_t WEBCONFIG_HOTKEY_ACTIVATION_TIME_MS = 50;
static const uint32_t WEBCONFIG_HOTKEY_HOLD_TIME_MS = 4000;

GP2040::GP2040() : nextRuntime(0) {
	Storage::getInstance().SetGamepad(new Gamepad(GAMEPAD_DEBOUNCE_MILLIS));
	Storage::getInstance().SetProcessedGamepad(new Gamepad(GAMEPAD_DEBOUNCE_MILLIS));
}

GP2040::~GP2040() {
}

void GP2040::setup() {
    // Setup Gamepad and Gamepad Storage
	Gamepad * gamepad = Storage::getInstance().GetGamepad();
	gamepad->setup();

	const BootAction bootAction = getBootAction();
	switch (bootAction) {
		case BootAction::ENTER_WEBCONFIG_MODE:
			{
				Storage::getInstance().SetConfigMode(true);
				initialize_driver(INPUT_MODE_CONFIG);
				ConfigManager::getInstance().setup(CONFIG_TYPE_WEB);
				break;	
			}

		case BootAction::ENTER_USB_MODE:
			{
				reset_usb_boot(0, 0);
				break;	
			}

		case BootAction::SET_INPUT_MODE_HID:
		case BootAction::SET_INPUT_MODE_SWITCH:
		case BootAction::SET_INPUT_MODE_XINPUT:
		case BootAction::SET_INPUT_MODE_PS4:
		case BootAction::SET_INPUT_MODE_KEYBOARD:
		case BootAction::NONE:
			{
				InputMode inputMode = gamepad->options.inputMode;
				if (bootAction == BootAction::SET_INPUT_MODE_HID) {
					inputMode = INPUT_MODE_HID;
				} else if (bootAction == BootAction::SET_INPUT_MODE_SWITCH) {
					inputMode = INPUT_MODE_SWITCH;
				} else if (bootAction == BootAction::SET_INPUT_MODE_XINPUT) {
					inputMode = INPUT_MODE_XINPUT;
				} else if (bootAction == BootAction::SET_INPUT_MODE_PS4) {
					inputMode = INPUT_MODE_PS4;
				} else if (bootAction == BootAction::SET_INPUT_MODE_KEYBOARD) {
					inputMode = INPUT_MODE_KEYBOARD;
				}

				if (inputMode != gamepad->options.inputMode) {
					// Save the changed input mode
					gamepad->options.inputMode = inputMode;
					gamepad->save();
				}

				initialize_driver(inputMode);
				break;
			}
	}

	// Initialize our ADC (various add-ons)
	adc_init();

	// Setup Add-ons
	// addons.LoadAddon(new AnalogInput(), CORE0_INPUT);
	// addons.LoadAddon(new BootselButtonAddon(), CORE0_INPUT);
	// addons.LoadAddon(new DualDirectionalInput(), CORE0_INPUT);
  	// addons.LoadAddon(new ExtraButtonAddon(), CORE0_INPUT);
	// addons.LoadAddon(new I2CAnalog1219Input(), CORE0_INPUT);
	// addons.LoadAddon(new JSliderInput(), CORE0_INPUT);
	// addons.LoadAddon(new ReverseInput(), CORE0_INPUT);
	// addons.LoadAddon(new TurboInput(), CORE0_INPUT);
	// addons.LoadAddon(new WiiExtensionInput(), CORE0_INPUT);
	// addons.LoadAddon(new PlayerNumAddon(), CORE0_USBREPORT);
	// addons.LoadAddon(new SliderSOCDInput(), CORE0_INPUT);
}

void GP2040::run() {
	Gamepad * gamepad = Storage::getInstance().GetGamepad();
	Gamepad * processedGamepad = Storage::getInstance().GetProcessedGamepad();
	bool configMode = Storage::getInstance().GetConfigMode();
	while (1) { // LOOP
		// Config Loop (Web-Config does not require gamepad)
		if (configMode == true) {
			ConfigManager& configManager = ConfigManager::getInstance();
			ConfigManager::getInstance().loop();

			gamepad->read();
			webConfigHotkey.process(gamepad, configMode);

			continue;
		}

		if (nextRuntime > getMicro()) { // fix for unsigned
			sleep_us(50); // Give some time back to our CPU (lower power consumption)
			continue;
		}

		// Gamepad Features
		gamepad->read(); 	// gpio pin reads
	#if GAMEPAD_DEBOUNCE_MILLIS > 0
		gamepad->debounce();
	#endif
		gamepad->hotkey(); 	// check for MPGS hotkeys
		webConfigHotkey.process(gamepad, configMode);

		// Pre-Process add-ons for MPGS
		addons.PreprocessAddons(ADDON_PROCESS::CORE0_INPUT);
		
		gamepad->process(); // process through MPGS

		// (Post) Process for add-ons
		addons.ProcessAddons(ADDON_PROCESS::CORE0_INPUT);

		// Copy Processed Gamepad for Core1 (race condition otherwise)
		memcpy(&processedGamepad->state, &gamepad->state, sizeof(GamepadState));

		// USB FEATURES : Send/Get USB Features (including Player LEDs on X-Input)
		send_report(gamepad->getReport(), gamepad->getReportSize());
		Storage::getInstance().ClearFeatureData();
		receive_report(Storage::getInstance().GetFeatureData());

		// Process USB Reports
		addons.ProcessAddons(ADDON_PROCESS::CORE0_USBREPORT);

		tud_task(); // TinyUSB Task update

		nextRuntime = getMicro() + GAMEPAD_POLL_MICRO;
	}
}

GP2040::BootAction GP2040::getBootAction() {
	switch (System::takeBootMode()) {
		case System::BootMode::GAMEPAD: return BootAction::NONE;
		case System::BootMode::WEBCONFIG: return BootAction::ENTER_WEBCONFIG_MODE;
		case System::BootMode::USB: return BootAction::ENTER_USB_MODE;
		case System::BootMode::DEFAULT:
			{
				// Determine boot action based on gamepad state during boot
				Gamepad * gamepad = Storage::getInstance().GetGamepad();

				// Mode selection window: the GBA screen shows the hold map and a
				// countdown, and the last mode button seen wins. The countdown
				// only advances on polls the GBA answered; if it never answers,
				// give up after linkWaitMs and keep the saved mode.
				const uint32_t modeButtonMask = GAMEPAD_MASK_B1 | GAMEPAD_MASK_B2
				                              | GAMEPAD_MASK_L1 | GAMEPAD_MASK_R1;
				uint8_t selectedMode = gamepad->options.inputMode;
				uint32_t heldButtons = 0;
				uint32_t heldDpad = 0;
				const int scanMs = 5000, stepMs = 10, linkWaitMs = 10000;
				int elapsed = 0, unanswered = 0;
				while (elapsed < scanMs) {
					uint32_t framesBefore = gba::validFrames;
					gba::modeScanWord = gba::modeScanFrame(selectedMode, (scanMs - elapsed + 999) / 1000);
					gamepad->read();
					uint32_t b = gamepad->state.buttons;
					if (b & GAMEPAD_MASK_B1)      selectedMode = INPUT_MODE_SWITCH;
					else if (b & GAMEPAD_MASK_B2) selectedMode = INPUT_MODE_XINPUT;
					else if (b & GAMEPAD_MASK_L1) selectedMode = INPUT_MODE_HID;
					else if (b & GAMEPAD_MASK_R1) selectedMode = INPUT_MODE_PS4;
					heldButtons |= b;
					heldDpad |= gamepad->state.dpad;
					sleep_ms(stepMs);
					if (gba::validFrames != framesBefore) {
						elapsed += stepMs;
					} else if (++unanswered * stepMs > linkWaitMs) {
						break;
					}
				}
				gba::modeScanWord = 0;

				// Present the pick to the checks below as a single mode button,
				// keeping other buttons seen so the bootloader combo still works
				uint32_t modeMask = 0;
				switch (selectedMode) {
					case INPUT_MODE_SWITCH: modeMask = GAMEPAD_MASK_B1; break;
					case INPUT_MODE_XINPUT: modeMask = GAMEPAD_MASK_B2; break;
					case INPUT_MODE_HID:    modeMask = GAMEPAD_MASK_L1; break;
					case INPUT_MODE_PS4:    modeMask = GAMEPAD_MASK_R1; break;
					default: break;
				}
				gamepad->state.buttons = modeMask | (heldButtons & ~modeButtonMask);
				gamepad->state.dpad = heldDpad;

				if (gamepad->pressedF1() && gamepad->pressedUp()) {
					return BootAction::ENTER_USB_MODE;
				}
				// else if (gamepad->pressedS2()) {
				// 	return BootAction::ENTER_WEBCONFIG_MODE;
				// }
				  else if (gamepad->pressedL1()) { // P1
					return BootAction::SET_INPUT_MODE_HID;
				} else if (gamepad->pressedR1()) { // P2
					return BootAction::SET_INPUT_MODE_PS4;
				} else if (gamepad->pressedB1()) { // K1
					return BootAction::SET_INPUT_MODE_SWITCH;
				} else if (gamepad->pressedB2()) { // K2
					return BootAction::SET_INPUT_MODE_XINPUT;
				} else if (gamepad->pressedR2()) { // K3
					return BootAction::SET_INPUT_MODE_KEYBOARD;
				} else {
					return BootAction::NONE;
				}

				break;
			}
	}

	return BootAction::NONE;
}

GP2040::WebConfigHotkey::WebConfigHotkey() :
	active(false),
	noButtonsPressedTimeout(nil_time),
	webConfigHotkeyMask(GAMEPAD_MASK_S2 | GAMEPAD_MASK_B3 | GAMEPAD_MASK_B4),
	webConfigHotkeyHoldTimeout(nil_time) {
}

void GP2040::WebConfigHotkey::process(Gamepad* gamepad, bool configMode) {
	// We only allow the hotkey to trigger after we observed no buttons pressed for a certain period of time.
	// We do this to avoid detecting buttons that are held during the boot process. In particular we want to avoid
	// oscillating between webconfig and default mode when the user keeps holding the hotkey buttons.
	if (!active) {
		if (gamepad->state.buttons == 0) {
			if (is_nil_time(noButtonsPressedTimeout)) {
				noButtonsPressedTimeout = make_timeout_time_us(WEBCONFIG_HOTKEY_ACTIVATION_TIME_MS);
			}

			if (time_reached(noButtonsPressedTimeout)) {
				active = true;
			}
		} else {
			noButtonsPressedTimeout = nil_time;
		}
	} else {
		if (gamepad->state.buttons == webConfigHotkeyMask) {
			if (is_nil_time(webConfigHotkeyHoldTimeout)) {
				webConfigHotkeyHoldTimeout = make_timeout_time_ms(WEBCONFIG_HOTKEY_HOLD_TIME_MS);
			}

			if (time_reached(webConfigHotkeyHoldTimeout)) {
				// If we are in webconfig mode we go to gamepad mode and vice versa
				System::reboot(configMode ? System::BootMode::GAMEPAD : System::BootMode::WEBCONFIG);
			}
		} else {
			webConfigHotkeyHoldTimeout = nil_time;
		}
	}
}
