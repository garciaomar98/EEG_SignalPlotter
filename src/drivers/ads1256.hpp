#ifndef PORTABLE_EEG_READER_DRIVERS_ADS1256_HPP_
#define PORTABLE_EEG_READER_DRIVERS_ADS1256_HPP_

#include <stdint.h>

namespace drivers {

struct Ads1256Config {
	uint8_t channel;
	uint8_t pga;
	uint32_t data_rate_sps;
};

class Ads1256 {
public:
	static Ads1256 &instance();

	int init();
	int readSample(int32_t &sample);
	Ads1256Config config() const;
	const char *channelName() const;

private:
	Ads1256() = default;
};

} // namespace drivers

#endif
