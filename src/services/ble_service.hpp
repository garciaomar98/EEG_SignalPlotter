#ifndef PORTABLE_EEG_READER_SERVICES_BLE_SERVICE_HPP_
#define PORTABLE_EEG_READER_SERVICES_BLE_SERVICE_HPP_

#include <stdint.h>

#include "drivers/ads1256.hpp"
#include "headband_channels.h"

namespace services {

enum class BleStatus : uint8_t {
	Off = 0U,
	Pairing,
	On,
};

class BleService {
public:
	int init(const drivers::Ads1256Config &adc_config);
	int requestPairing();
	BleStatus status() const;
	bool pairingActive() const;
	uint32_t pairingRemainingMs(uint32_t now_ms) const;
	void tick(uint32_t now_ms);
	int sendAdcCsvRow(uint32_t uptime_ms, const headband::SampleBuffer &samples);
};

} // namespace services

#endif
