#include "services/signal_processing_service.hpp"

#include <errno.h>

namespace services {

int SignalProcessingService::init()
{
	return 0;
}

headband::SampleBuffer SignalProcessingService::processSample(int32_t sample)
{
	headband::SampleBuffer samples = {};

	samples.values[0] = sample;
	return samples;
}

} // namespace services
