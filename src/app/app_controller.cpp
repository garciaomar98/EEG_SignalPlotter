#include <errno.h>
#include <inttypes.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "app/app_controller.hpp"
#include "app/app_events.hpp"
#include "headband_channels.h"
#include "ui/ui.hpp"

#define DISPLAY_BL_NODE DT_NODELABEL(display_bl)
#define BATTERY_INPUT_NODE DT_NODELABEL(battery_input)
#define PAIR_BUTTON_NODE DT_ALIAS(sw0)

LOG_MODULE_REGISTER(app_controller, LOG_LEVEL_INF);

BUILD_ASSERT(DT_NODE_EXISTS(BATTERY_INPUT_NODE),
	     "Board DTS must provide a battery_input node");
BUILD_ASSERT(DT_NODE_HAS_STATUS(BATTERY_INPUT_NODE, okay),
	     "battery_input must be enabled");
BUILD_ASSERT(DT_NODE_HAS_PROP(BATTERY_INPUT_NODE, io_channels),
	     "battery_input must provide io-channels");
BUILD_ASSERT(DT_PROP_LEN(BATTERY_INPUT_NODE, io_channels) == 1,
	     "battery_input must define exactly one io-channel");
BUILD_ASSERT(DT_NODE_HAS_PROP(BATTERY_INPUT_NODE, power_gpios),
	     "battery_input must define power-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(BATTERY_INPUT_NODE, output_ohms),
	     "battery_input must define output-ohms");
BUILD_ASSERT(DT_NODE_HAS_PROP(BATTERY_INPUT_NODE, full_ohms),
	     "battery_input must define full-ohms");
BUILD_ASSERT(DT_NODE_EXISTS(PAIR_BUTTON_NODE),
	     "Board DTS must provide the sw0 alias for BLE pairing");

namespace {

constexpr uint32_t kChannelMask = 0x01U;
constexpr uint32_t kBlePairLongPressMs = 800U;
constexpr uint32_t kBlePairingWindowMs = 30000U;
constexpr uint32_t kPowerSourcePollMs = 1000U;
constexpr uint32_t kBatterySampleIntervalMs = 5000U;
constexpr uint8_t kBatteryWarmupSamples = 2U;
constexpr uint8_t kBatteryAverageSamples = 8U;
constexpr uint32_t kUiTickIntervalMs = 16U;
constexpr int kBatteryLevelUnknown = -1;
constexpr int32_t kBatteryMillivoltsUnknown = -1;

const struct gpio_dt_spec kDisplayBacklight = GPIO_DT_SPEC_GET(DISPLAY_BL_NODE, gpios);
const struct gpio_dt_spec kBatteryEnable = GPIO_DT_SPEC_GET(BATTERY_INPUT_NODE, power_gpios);
const struct gpio_dt_spec kPairButton = GPIO_DT_SPEC_GET(PAIR_BUTTON_NODE, gpios);
const struct adc_dt_spec kBatterySense = ADC_DT_SPEC_GET(BATTERY_INPUT_NODE);
constexpr uint32_t kBatterySenseSettleUs = DT_PROP_OR(BATTERY_INPUT_NODE, power_on_sample_delay_us, 100U);
constexpr int32_t kBatteryDividerOutputOhms = DT_PROP(BATTERY_INPUT_NODE, output_ohms);
constexpr int32_t kBatteryDividerFullOhms = DT_PROP(BATTERY_INPUT_NODE, full_ohms);

struct BatteryCurvePoint {
	int32_t millivolts;
	int percent;
};

struct PairButtonState {
	bool ready;
	bool was_pressed;
	bool pairing_requested;
	uint32_t pressed_since_ms;
};

constexpr BatteryCurvePoint kBatteryCurve[] = {
	{4000, 100},
	{3900, 80},
	{3800, 60},
	{3700, 40},
	{3600, 25},
	{3500, 10},
	{3300, 0},
};

int init_display_backlight()
{
	if (!gpio_is_ready_dt(&kDisplayBacklight)) {
		return -ENODEV;
	}

	const int err = gpio_pin_configure_dt(&kDisplayBacklight, GPIO_OUTPUT_ACTIVE);

	if (err == 0) {
		(void)gpio_pin_set_dt(&kDisplayBacklight, 1);
	}

	return err;
}

int init_pair_button()
{
	if (!gpio_is_ready_dt(&kPairButton)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&kPairButton, GPIO_INPUT);
}

void handle_pair_button(PairButtonState *button, uint32_t now)
{
	int pressed_raw;
	bool pressed;

	if ((button == nullptr) || !button->ready) {
		return;
	}

	pressed_raw = gpio_pin_get_dt(&kPairButton);
	if (pressed_raw < 0) {
		return;
	}

	pressed = pressed_raw > 0;

	if (pressed && !button->was_pressed) {
		button->pressed_since_ms = now;
		button->pairing_requested = false;
	}

	if (!pressed) {
		button->pressed_since_ms = 0U;
		button->pairing_requested = false;
	}

	if (pressed && !button->pairing_requested &&
	    ((now - button->pressed_since_ms) >= kBlePairLongPressMs)) {
		button->pairing_requested = true;
		printk("BLE pair button long press detected\n");
		(void)app::publish({
			.type = app::EventType::PairingRequested,
			.timestamp_ms = now,
			.samples = {},
			.error = 0,
		});
	}

	button->was_pressed = pressed;
}

void set_unknown_battery_state(app::BatteryState *state)
{
	if (state == nullptr) {
		return;
	}

	state->valid = false;
	state->source_index = -1;
	state->channel_id = -1;
	state->raw_code = 0;
	state->millivolts = kBatteryMillivoltsUnknown;
	state->percent = kBatteryLevelUnknown;
}

int battery_percent_from_millivolts(int32_t millivolts)
{
	if (millivolts < 0) {
		return kBatteryLevelUnknown;
	}

	if (millivolts >= kBatteryCurve[0].millivolts) {
		return kBatteryCurve[0].percent;
	}

	const size_t last = ARRAY_SIZE(kBatteryCurve) - 1U;
	if (millivolts <= kBatteryCurve[last].millivolts) {
		return kBatteryCurve[last].percent;
	}

	for (size_t index = 0U; index < last; ++index) {
		const BatteryCurvePoint upper = kBatteryCurve[index];
		const BatteryCurvePoint lower = kBatteryCurve[index + 1U];

		if (millivolts < lower.millivolts) {
			continue;
		}

		const int32_t span_mv = upper.millivolts - lower.millivolts;
		const int32_t span_percent = upper.percent - lower.percent;
		const int32_t offset_mv = millivolts - lower.millivolts;

		return lower.percent +
		       static_cast<int>((span_percent * offset_mv + (span_mv / 2)) / span_mv);
	}

	return kBatteryLevelUnknown;
}

int init_battery_input()
{
	if (!gpio_is_ready_dt(&kBatteryEnable) || !adc_is_ready_dt(&kBatterySense)) {
		return -ENODEV;
	}

	int err = gpio_pin_configure_dt(&kBatteryEnable, GPIO_INPUT);
	if (err != 0) {
		return err;
	}

	err = adc_channel_setup_dt(&kBatterySense);
	if (err != 0) {
		return err;
	}

	printk("Battery sense ready: adc=%s channel=%u enable=%s pin=%u\n",
	       kBatterySense.dev->name,
	       static_cast<unsigned int>(kBatterySense.channel_id),
	       kBatteryEnable.port->name,
	       static_cast<unsigned int>(kBatteryEnable.pin));
	return 0;
}

int detect_usb_power(bool *usb_present)
{
	if (usb_present == nullptr) {
		return -EINVAL;
	}

	int err = gpio_pin_configure_dt(&kBatteryEnable, GPIO_INPUT);
	if (err != 0) {
		return err;
	}

	k_busy_wait(50U);

	const int value = gpio_pin_get_dt(&kBatteryEnable);
	if (value < 0) {
		return value;
	}

	/*
	 * TTGO T-Display V1.1 documentation indicates GPIO14 is automatically
	 * driven high while USB power is present, and otherwise must be driven
	 * high by firmware to enable the battery divider on GPIO34.
	 */
	*usb_present = (value != 0);

	if (!*usb_present) {
		err = gpio_pin_configure_dt(&kBatteryEnable, GPIO_OUTPUT_INACTIVE);
		if (err != 0) {
			return err;
		}
	}

	return 0;
}

int32_t battery_cell_millivolts_from_sense(int32_t sense_millivolts)
{
	if (sense_millivolts < 0) {
		return kBatteryMillivoltsUnknown;
	}

	return static_cast<int32_t>(
		(static_cast<int64_t>(sense_millivolts) * kBatteryDividerFullOhms) /
		kBatteryDividerOutputOhms);
}

int read_battery_state(app::BatteryState *state)
{
	struct adc_sequence sequence = {};
	uint32_t raw_accumulator = 0U;
	uint16_t raw_code = 0U;
	int32_t sense_millivolts = 0;
	int err = gpio_pin_set_dt(&kBatteryEnable, 1);

	if (state == nullptr) {
		return -EINVAL;
	}

	if (err != 0) {
		return err;
	}

	err = gpio_pin_configure_dt(&kBatteryEnable, GPIO_OUTPUT_ACTIVE);
	if (err != 0) {
		return err;
	}

	k_busy_wait(kBatterySenseSettleUs);

	for (uint8_t sample = 0U;
	     sample < (kBatteryWarmupSamples + kBatteryAverageSamples);
	     ++sample) {
		err = adc_sequence_init_dt(&kBatterySense, &sequence);
		if (err != 0) {
			break;
		}

		sequence.buffer = &raw_code;
		sequence.buffer_size = sizeof(raw_code);

		err = adc_read_dt(&kBatterySense, &sequence);
		if (err != 0) {
			break;
		}

		if (sample >= kBatteryWarmupSamples) {
			raw_accumulator += raw_code;
		}
	}

	const int disable_err = gpio_pin_set_dt(&kBatteryEnable, 0);
	if ((err == 0) && (disable_err != 0)) {
		err = disable_err;
	}

	if (err != 0) {
		return err;
	}

	raw_code = static_cast<uint16_t>(
		(raw_accumulator + (kBatteryAverageSamples / 2U)) / kBatteryAverageSamples);
	sense_millivolts = raw_code;
	err = adc_raw_to_millivolts_dt(&kBatterySense, &sense_millivolts);
	if (err != 0) {
		return err;
	}

	state->valid = true;
	state->source_index = 0;
	state->channel_id = static_cast<int32_t>(kBatterySense.channel_id);
	state->raw_code = raw_code;
	state->millivolts = battery_cell_millivolts_from_sense(sense_millivolts);
	state->percent = battery_percent_from_millivolts(state->millivolts);

	printk("Battery raw=%" PRId32 " sense_mv=%" PRId32 " vbatt_mv=%" PRId32
	       " percent=%" PRId32 "\n",
	       state->raw_code, sense_millivolts, state->millivolts, state->percent);
	return 0;
}

ui::BleStatus to_ui_ble_status(services::BleStatus status)
{
	switch (status) {
	case services::BleStatus::On:
		return ui::BleStatus::On;
	case services::BleStatus::Pairing:
		return ui::BleStatus::Pairing;
	case services::BleStatus::Off:
	default:
		return ui::BleStatus::Off;
	}
}

} // namespace

namespace app {

Controller &Controller::instance()
{
	static Controller controller;

	return controller;
}

int Controller::init()
{
	headband::SampleBuffer samples = {};
	BatteryState battery_state = {};
	services::BleStatus ble_status = services::BleStatus::Off;
	bool adc_ready = false;
	bool battery_ready = false;
	bool charging = false;
	bool ui_ready = false;
	int err;

	set_unknown_battery_state(&battery_state);
	setBatteryState(battery_state);

	err = init_display_backlight();
	if (err != 0) {
		printk("Display backlight init failed: %d\n", err);
	}

	err = init_pair_button();
	if (err != 0) {
		printk("Pair button init failed: %d\n", err);
	}

	err = init_battery_input();
	if (err != 0) {
		printk("Battery init failed: %d\n", err);
	} else {
		battery_ready = true;
		err = detect_usb_power(&charging);
		if (err != 0) {
			printk("USB power detect failed: %d\n", err);
			set_unknown_battery_state(&battery_state);
		} else if (!charging) {
			err = read_battery_state(&battery_state);
			if (err != 0) {
				printk("Battery read failed: %d\n", err);
				set_unknown_battery_state(&battery_state);
			}
		} else {
			set_unknown_battery_state(&battery_state);
		}
		setBatteryState(battery_state);
	}

	err = acquisition_.init();
	if (err != 0) {
		printk("ADS1256 init failed: %d\n", err);
	} else {
		adc_ready = true;
		printk("ADS1256 ready channel=%s gain=%u rate=%uSPS\n",
		       acquisition_.channelName(),
		       static_cast<unsigned int>(acquisition_.config().pga),
		       static_cast<unsigned int>(acquisition_.config().data_rate_sps));
		acquisition_.emitCsvInfo();
	}

	err = signal_processing_.init();
	if (err != 0) {
		printk("Signal processing init failed: %d\n", err);
	}

	err = ble_.init(acquisition_.config());
	if (err != 0) {
		printk("BLE init failed: %d\n", err);
	} else {
		ble_status = ble_.status();
	}

	err = ui::init(kChannelMask);
	if (err != 0) {
		printk("UI init failed: %d\n", err);
	} else {
		ui_ready = true;
		ui::set_charging(charging);
		ui::set_adc_ready(adc_ready && !charging);
		ui::set_battery_level(batteryPercent());
		ui::set_ble_status(to_ui_ble_status(ble_status));
		ui::set_pairing_progress(false, 0U, kBlePairingWindowMs);
		ui::set_signal_quality(charging ? "USB power detected" : "ADS1256 linked");
		ui::update_samples(samples);
	}

	adc_ready_ = adc_ready;
	battery_ready_ = battery_ready;
	charging_ = charging;
	ui_ready_ = ui_ready;
	transitionTo((adc_ready_ && !charging_) ? AppState::Acquiring : AppState::Idle);
	return 0;
}

void Controller::run()
{
	headband::SampleBuffer samples = {};
	BatteryState battery_state = batteryState();
	PairButtonState pair_button = {};
	const uint32_t adc_interval_ms = acquisition_.sampleIntervalMs();
	uint32_t last_sample_tick = 0U;
	uint32_t last_power_tick = 0U;
	uint32_t last_battery_tick = 0U;
	uint32_t last_ui_tick = 0U;
	services::BleStatus ble_status = ble_.status();
	bool adc_ready = adc_ready_;
	bool battery_ready = battery_ready_;
	bool charging = charging_;
	bool ui_ready = ui_ready_;
	int err;

	if (init_pair_button() == 0) {
		pair_button.ready = true;
	}

	while (true) {
		const uint32_t now = k_uptime_get_32();
		Event event = {};

		handle_pair_button(&pair_button, now);
		ble_.tick(now);

		while (wait(event, K_NO_WAIT) == 0) {
			handleEvent(event);
		}

		const bool pairing_active = ble_.pairingActive();
		const uint32_t pairing_remaining_ms = ble_.pairingRemainingMs(now);

		const services::BleStatus current_ble_status = ble_.status();

		if (current_ble_status != ble_status) {
			ble_status = current_ble_status;
			if (ui_ready) {
				ui::set_ble_status(to_ui_ble_status(ble_status));
				if (ble_status != services::BleStatus::Pairing) {
					ui::set_pairing_progress(false, 0U, kBlePairingWindowMs);
				}
			}
		}

		if (battery_ready && ((now - last_power_tick) >= kPowerSourcePollMs)) {
			bool usb_present = charging;

			last_power_tick = now;
			err = detect_usb_power(&usb_present);
			if (err != 0) {
				printk("USB power detect failed: %d\n", err);
			} else if (usb_present != charging) {
				charging = usb_present;
				printk("Power source changed: %s\n", charging ? "USB" : "battery");
				transitionTo((adc_ready && !charging) ? AppState::Acquiring : AppState::Idle);

				if (charging) {
					set_unknown_battery_state(&battery_state);
				} else {
					err = read_battery_state(&battery_state);
					if (err != 0) {
						printk("Battery read failed: %d\n", err);
						set_unknown_battery_state(&battery_state);
					}
				}

				setBatteryState(battery_state);
				if (ui_ready) {
					ui::set_charging(charging);
					ui::set_adc_ready(adc_ready && !charging);
					ui::set_battery_level(batteryPercent());
					ui::set_signal_quality(charging ? "USB power detected" :
								    "ADS1256 linked");
				}
			}
		}

		if (!charging && adc_ready && (state_ != AppState::Idle) &&
		    ((now - last_sample_tick) >= adc_interval_ms)) {
			int32_t sample = 0;

			last_sample_tick = now;
			err = acquisition_.readSample(sample);
			if (err != 0) {
				printk("ADS1256 read failed: %d\n", err);
				adc_ready = false;
				transitionTo(AppState::Error);
				if (ui_ready) {
					ui::set_adc_ready(false);
					ui::set_signal_quality("ADS1256 offline");
				}
			} else {
				samples = signal_processing_.processSample(sample);
				printk("ADC_RAW,%" PRIu32 ",%" PRId32 "\n", now, sample);
				err = ble_.sendAdcCsvRow(now, samples);
				if (err != 0) {
					printk("BLE sample send failed: %d\n", err);
				}
				(void)publish({
					.type = EventType::SampleReady,
					.timestamp_ms = now,
					.samples = samples,
					.error = 0,
				});

				if (ui_ready) {
					ui::update_samples(samples);
					ui::set_signal_quality("ADS1256 streaming");
				}
			}
		}

		if (!charging && battery_ready &&
		    ((now - last_battery_tick) >= kBatterySampleIntervalMs)) {
			last_battery_tick = now;
			err = read_battery_state(&battery_state);
			if (err != 0) {
				printk("Battery read failed: %d\n", err);
				set_unknown_battery_state(&battery_state);
			}

			setBatteryState(battery_state);
			if (ui_ready) {
				ui::set_battery_level(batteryPercent());
			}
		}

		if (ui_ready && ((now - last_ui_tick) >= kUiTickIntervalMs)) {
			last_ui_tick = now;
			ui::set_pairing_progress(pairing_active, pairing_remaining_ms,
						 kBlePairingWindowMs);
			ui::tick();
		}

		k_msleep(1);
	}
}

void Controller::handleEvent(const Event &event)
{
	switch (event.type) {
	case EventType::PairingRequested:
		if (ble_.requestPairing() != 0) {
			printk("BLE pairing request failed\n");
		}
		break;
	case EventType::SampleReady:
		if (state_ == AppState::Acquiring) {
			transitionTo(AppState::Streaming);
		}
		break;
	case EventType::StartAcquisitionRequested:
		transitionTo(AppState::Acquiring);
		break;
	case EventType::StopAcquisitionRequested:
		transitionTo(AppState::Idle);
		break;
	case EventType::Error:
		transitionTo(AppState::Error);
		break;
	default:
		break;
	}
}

void Controller::transitionTo(AppState new_state)
{
	if (state_ == new_state) {
		return;
	}

	printk("App state: %s -> %s\n", stateName(state_), stateName(new_state));
	state_ = new_state;
}

} // namespace app
