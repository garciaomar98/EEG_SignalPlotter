#ifndef PORTABLE_EEG_READER_APP_APP_EVENTS_HPP_
#define PORTABLE_EEG_READER_APP_APP_EVENTS_HPP_

#include <stdint.h>

#include <zephyr/kernel.h>

#include "headband_channels.h"

namespace app {

enum class EventType : uint8_t {
	StartAcquisitionRequested = 0U,
	StopAcquisitionRequested,
	AcquisitionStarted,
	AcquisitionStopped,
	SampleReady,
	SampleBlockReady,
	BleConnected,
	BleDisconnected,
	PairingRequested,
	Error,
};

struct Event {
	EventType type;
	uint32_t timestamp_ms;
	headband::SampleBuffer samples;
	int error;
};

int publish(const Event &event);
int wait(Event &event, k_timeout_t timeout);

} // namespace app

#endif
