#ifndef PORTABLE_EEG_READER_SERVICES_SIGNAL_PROCESSING_SERVICE_HPP_
#define PORTABLE_EEG_READER_SERVICES_SIGNAL_PROCESSING_SERVICE_HPP_

#include "headband_channels.h"

namespace services {

class SignalProcessingService {
public:
	int init();
	headband::SampleBuffer processSample(int32_t sample);
};

} // namespace services

#endif
