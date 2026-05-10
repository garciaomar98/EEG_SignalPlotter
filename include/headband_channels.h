#ifndef HEADBAND_CHANNELS_H_
#define HEADBAND_CHANNELS_H_

#include <stddef.h>
#include <stdint.h>

namespace headband {

constexpr size_t kMaxPlotChannels = 1U;

struct SampleBuffer {
	int32_t values[kMaxPlotChannels];
};

inline void clear_samples(SampleBuffer *samples)
{
	for (size_t channel = 0U; channel < kMaxPlotChannels; ++channel) {
		samples->values[channel] = 0;
	}
}

} // namespace headband

#endif
