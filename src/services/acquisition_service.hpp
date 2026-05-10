#ifndef PORTABLE_EEG_READER_SERVICES_ACQUISITION_SERVICE_HPP_
#define PORTABLE_EEG_READER_SERVICES_ACQUISITION_SERVICE_HPP_

#include <stdint.h>

#include "drivers/ads1256.hpp"

namespace services {

class AcquisitionService {
public:
	int init();
	int readSample(int32_t &sample);
	uint32_t sampleIntervalMs() const;
	drivers::Ads1256Config config() const;
	const char *channelName() const;
	void emitCsvInfo() const;
	bool ready() const;

private:
	bool ready_ = false;
};

} // namespace services

#endif
