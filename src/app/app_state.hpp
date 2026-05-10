#ifndef PORTABLE_EEG_READER_APP_APP_STATE_HPP_
#define PORTABLE_EEG_READER_APP_APP_STATE_HPP_

#include <stdint.h>

namespace app {

enum class AppState : uint8_t {
	Boot = 0U,
	Idle,
	Acquiring,
	Streaming,
	Error,
};

struct BatteryState {
	bool valid;
	int32_t source_index;
	int32_t channel_id;
	int32_t raw_code;
	int32_t millivolts;
	int32_t percent;
};

void setBatteryState(const BatteryState &state);
BatteryState batteryState();
int32_t batteryPercent();
bool batteryValid();
const char *stateName(AppState state);

} // namespace app

#endif
