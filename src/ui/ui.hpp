#ifndef PORTABLE_EEG_READER_UI_UI_HPP_
#define PORTABLE_EEG_READER_UI_UI_HPP_

#include <stdint.h>

#include "headband_channels.h"

namespace ui {

enum class BleStatus : uint8_t {
	Off,
	Pairing,
	On,
};

int init(uint32_t channel_mask);
void set_adc_ready(bool ready);
void set_battery_level(int percent);
void set_charging(bool charging);
void set_ble_status(BleStatus status);
void set_ble_connected(bool connected);
void set_pairing_progress(bool active, uint32_t remaining_ms, uint32_t timeout_ms);
void set_signal_quality(const char *text);
void update_samples(const headband::SampleBuffer &samples);
void tick();

} // namespace ui

#endif
