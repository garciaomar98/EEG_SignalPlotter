#include "services/acquisition_service.hpp"

#include <errno.h>
#include <inttypes.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(acquisition_service, LOG_LEVEL_INF);

namespace services {
namespace {

constexpr uint32_t kDefaultSampleIntervalMs = 100U;
constexpr int32_t kAds1256MinRaw = -8388608;
constexpr int32_t kAds1256MaxRaw = 8388607;

} // namespace

int AcquisitionService::init()
{
	const int err = drivers::Ads1256::instance().init();

	ready_ = (err == 0);
	return err;
}

int AcquisitionService::readSample(int32_t &sample)
{
	if (!ready_) {
		return -ENODEV;
	}

	return drivers::Ads1256::instance().readSample(sample);
}

uint32_t AcquisitionService::sampleIntervalMs() const
{
	const drivers::Ads1256Config adc_config = config();

	if (adc_config.data_rate_sps == 0U) {
		return kDefaultSampleIntervalMs;
	}

	const uint32_t interval = 1000U / adc_config.data_rate_sps;
	return (interval == 0U) ? 1U : interval;
}

drivers::Ads1256Config AcquisitionService::config() const
{
	return drivers::Ads1256::instance().config();
}

const char *AcquisitionService::channelName() const
{
	return drivers::Ads1256::instance().channelName();
}

void AcquisitionService::emitCsvInfo() const
{
	const drivers::Ads1256Config adc_config = config();

	printk("ADC_RAW_INFO,adc0,channel_id,%u,resolution,24,min_raw,%" PRId32
	       ",max_raw,%" PRId32 ",gain,%u,data_rate,%u\n",
	       static_cast<unsigned int>(adc_config.channel),
	       kAds1256MinRaw, kAds1256MaxRaw,
	       static_cast<unsigned int>(adc_config.pga),
	       static_cast<unsigned int>(adc_config.data_rate_sps));
	printk("ADC_RAW_HEADER,uptime_ms,adc0\n");
}

bool AcquisitionService::ready() const
{
	return ready_;
}

} // namespace services
