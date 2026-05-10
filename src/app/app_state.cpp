#include "app/app_state.hpp"

#include <zephyr/sys/atomic.h>

namespace app {
namespace {

atomic_t g_battery_valid = ATOMIC_INIT(0);
atomic_t g_battery_source_index = ATOMIC_INIT(-1);
atomic_t g_battery_channel_id = ATOMIC_INIT(-1);
atomic_t g_battery_raw_code = ATOMIC_INIT(0);
atomic_t g_battery_millivolts = ATOMIC_INIT(-1);
atomic_t g_battery_percent = ATOMIC_INIT(-1);

} // namespace

void setBatteryState(const BatteryState &state)
{
	atomic_set(&g_battery_source_index, state.source_index);
	atomic_set(&g_battery_channel_id, state.channel_id);
	atomic_set(&g_battery_raw_code, state.raw_code);
	atomic_set(&g_battery_millivolts, state.millivolts);
	atomic_set(&g_battery_percent, state.percent);
	atomic_set(&g_battery_valid, state.valid ? 1 : 0);
}

BatteryState batteryState()
{
	return {
		.valid = atomic_get(&g_battery_valid) != 0,
		.source_index = atomic_get(&g_battery_source_index),
		.channel_id = atomic_get(&g_battery_channel_id),
		.raw_code = atomic_get(&g_battery_raw_code),
		.millivolts = atomic_get(&g_battery_millivolts),
		.percent = atomic_get(&g_battery_percent),
	};
}

int32_t batteryPercent()
{
	return atomic_get(&g_battery_percent);
}

bool batteryValid()
{
	return atomic_get(&g_battery_valid) != 0;
}

const char *stateName(AppState state)
{
	switch (state) {
	case AppState::Boot:
		return "Boot";
	case AppState::Idle:
		return "Idle";
	case AppState::Acquiring:
		return "Acquiring";
	case AppState::Streaming:
		return "Streaming";
	case AppState::Error:
	default:
		return "Error";
	}
}

} // namespace app
