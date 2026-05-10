#include "drivers/ads1256.hpp"

#include <errno.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define ADS1256_NODE DT_NODELABEL(ads1256)

#if !DT_NODE_HAS_STATUS(ADS1256_NODE, okay)
#error "Board DTS must provide an ads1256 node"
#endif

namespace drivers {
namespace {

constexpr uint8_t kCommandRdata = 0x01U;
constexpr uint8_t kCommandRreg = 0x10U;
constexpr uint8_t kCommandRdatac = 0x03U;
constexpr uint8_t kCommandSdatac = 0x0FU;
constexpr uint8_t kCommandSelfcal = 0xF0U;
constexpr uint8_t kCommandReset = 0xFEU;

constexpr uint8_t kRegisterStatus = 0x00U;
constexpr uint8_t kRegisterMux = 0x01U;
constexpr uint8_t kRegisterAdcon = 0x02U;
constexpr uint8_t kRegisterDrate = 0x03U;

constexpr uint8_t kStatusMsbFirstNoAutoCalNoBuffer = 0x00U;
constexpr uint8_t kMuxAincom = 0x0FU;
constexpr uint32_t kCommandDelayUs = 10U;
constexpr uint32_t kResetPulseMs = 2U;
constexpr uint32_t kResetRecoveryMs = 5U;
constexpr uint32_t kInitTimeoutMs = 500U;

struct DataRateEntry {
	uint32_t sps;
	uint8_t reg;
};

constexpr DataRateEntry kDataRates[] = {
	{2U, 0x03U},
	{5U, 0x13U},
	{10U, 0x23U},
	{15U, 0x33U},
	{25U, 0x43U},
	{30U, 0x53U},
	{50U, 0x63U},
	{60U, 0x72U},
	{100U, 0x82U},
	{500U, 0x92U},
	{1000U, 0xA1U},
	{2000U, 0xB0U},
	{3750U, 0xC0U},
	{7500U, 0xD0U},
	{15000U, 0xE0U},
	{30000U, 0xF0U},
};

const struct spi_dt_spec kSpi =
	SPI_DT_SPEC_GET(ADS1256_NODE,
			SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPHA);
const struct gpio_dt_spec kDrdy = GPIO_DT_SPEC_GET(ADS1256_NODE, drdy_gpios);
const struct gpio_dt_spec kReset = GPIO_DT_SPEC_GET_OR(ADS1256_NODE, reset_gpios, {0});
const struct gpio_dt_spec kSyncPwdn =
	GPIO_DT_SPEC_GET_OR(ADS1256_NODE, syncpwdn_gpios, {0});

Ads1256Config g_config = {
	.channel = DT_PROP(ADS1256_NODE, single_ended_channel),
	.pga = DT_PROP(ADS1256_NODE, pga),
	.data_rate_sps = DT_PROP(ADS1256_NODE, data_rate),
};

bool gpio_spec_present(const struct gpio_dt_spec &spec)
{
	return (spec.port != nullptr);
}

int wait_drdy(uint32_t timeout_ms)
{
	const int64_t deadline = k_uptime_get() + timeout_ms;

	while (k_uptime_get() < deadline) {
		const int value = gpio_pin_get_dt(&kDrdy);

		if (value < 0) {
			return value;
		}

		if (value != 0) {
			return 0;
		}

		k_sleep(K_USEC(100));
	}

	return -ETIMEDOUT;
}

int spi_write_bytes(const uint8_t *data, size_t size)
{
	const struct spi_buf tx_buf = {
		.buf = const_cast<uint8_t *>(data),
		.len = size,
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1U,
	};

	return spi_write_dt(&kSpi, &tx);
}

int spi_write_with_config(const struct spi_config *config, const uint8_t *data, size_t size)
{
	const struct spi_buf tx_buf = {
		.buf = const_cast<uint8_t *>(data),
		.len = size,
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1U,
	};

	return spi_write(kSpi.bus, config, &tx);
}

int spi_transceive_with_config(const struct spi_config *config,
			       const uint8_t *tx_data,
			       uint8_t *rx_data,
			       size_t size)
{
	const struct spi_buf tx_buf = {
		.buf = const_cast<uint8_t *>(tx_data),
		.len = size,
	};
	const struct spi_buf rx_buf = {
		.buf = rx_data,
		.len = size,
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1U,
	};
	const struct spi_buf_set rx = {
		.buffers = &rx_buf,
		.count = 1U,
	};

	return spi_transceive(kSpi.bus, config, &tx, &rx);
}

int write_command(uint8_t command)
{
	const int err = spi_write_bytes(&command, sizeof(command));

	if (err == 0) {
		k_busy_wait(kCommandDelayUs);
	}

	return err;
}

int write_register(uint8_t reg, uint8_t value)
{
	const uint8_t bytes[] = {
		static_cast<uint8_t>(0x50U | reg),
		0x00U,
		value,
	};
	const int err = spi_write_bytes(bytes, sizeof(bytes));

	if (err == 0) {
		k_busy_wait(kCommandDelayUs);
	}

	return err;
}

int read_register(uint8_t reg, uint8_t *value)
{
	struct spi_config config = kSpi.config;
	const uint8_t command[] = {
		static_cast<uint8_t>(kCommandRreg | reg),
		0x00U,
	};
	const uint8_t dummy_tx[] = {0xFFU};
	uint8_t dummy_rx[1] = {};
	int err;

	if (value == nullptr) {
		return -EINVAL;
	}

	config.operation |= SPI_HOLD_ON_CS | SPI_LOCK_ON;

	err = spi_write_with_config(&config, command, sizeof(command));
	if (err == 0) {
		k_busy_wait(kCommandDelayUs);
		err = spi_transceive_with_config(&config, dummy_tx, dummy_rx, sizeof(dummy_tx));
	}

	(void)spi_release(kSpi.bus, &config);

	if (err != 0) {
		return err;
	}

	*value = dummy_rx[0];
	return 0;
}

uint8_t data_rate_register(uint32_t sps)
{
	for (const DataRateEntry &entry : kDataRates) {
		if (entry.sps == sps) {
			return entry.reg;
		}
	}

	return 0x23U;
}

uint8_t pga_bits(uint8_t gain)
{
	switch (gain) {
	case 1U:
		return 0U;
	case 2U:
		return 1U;
	case 4U:
		return 2U;
	case 8U:
		return 3U;
	case 16U:
		return 4U;
	case 32U:
		return 5U;
	case 64U:
		return 6U;
	default:
		return 0U;
	}
}

uint8_t mux_register(uint8_t channel)
{
	return static_cast<uint8_t>((channel << 4) | kMuxAincom);
}

int configure_optional_gpio(const struct gpio_dt_spec &spec, gpio_flags_t extra_flags,
			    int initial_active)
{
	if (!gpio_spec_present(spec)) {
		return 0;
	}

	if (!gpio_is_ready_dt(&spec)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&spec, extra_flags |
					    (initial_active != 0 ? GPIO_OUTPUT_ACTIVE
								 : GPIO_OUTPUT_INACTIVE));
}

void hardware_reset()
{
	if (!gpio_spec_present(kReset)) {
		return;
	}

	(void)gpio_pin_set_dt(&kReset, 0);
	k_msleep(1);
	(void)gpio_pin_set_dt(&kReset, 1);
	k_msleep(kResetPulseMs);
	(void)gpio_pin_set_dt(&kReset, 0);
	k_msleep(kResetRecoveryMs);
}

int sign_extend_24(const uint8_t bytes[3])
{
	int32_t value = (static_cast<int32_t>(bytes[0]) << 16) |
			(static_cast<int32_t>(bytes[1]) << 8) |
			static_cast<int32_t>(bytes[2]);

	if ((value & 0x00800000L) != 0) {
		value |= static_cast<int32_t>(0xFF000000UL);
	}

	return value;
}

} // namespace

Ads1256 &Ads1256::instance()
{
	static Ads1256 instance;

	return instance;
}

int Ads1256::init()
{
	uint8_t status_reg = 0U;
	uint8_t mux_reg = 0U;
	uint8_t adcon_reg = 0U;
	uint8_t drate_reg = 0U;

	if (!spi_is_ready_dt(&kSpi)) {
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&kDrdy)) {
		return -ENODEV;
	}

	int err = gpio_pin_configure_dt(&kDrdy, GPIO_INPUT);
	if (err != 0) {
		return err;
	}

	err = configure_optional_gpio(kReset, GPIO_OUTPUT, 0);
	if (err != 0) {
		return err;
	}

	err = configure_optional_gpio(kSyncPwdn, GPIO_OUTPUT, 0);
	if (err != 0) {
		return err;
	}

	hardware_reset();

	if (gpio_spec_present(kSyncPwdn)) {
		(void)gpio_pin_set_dt(&kSyncPwdn, 0);
	}

	err = write_command(kCommandReset);
	if (err != 0) {
		return err;
	}

	k_msleep(kResetRecoveryMs);

	err = wait_drdy(kInitTimeoutMs);
	if (err != 0) {
		return err;
	}

	err = write_command(kCommandSdatac);
	if (err != 0) {
		return err;
	}

	err = write_register(kRegisterStatus, kStatusMsbFirstNoAutoCalNoBuffer);
	if (err != 0) {
		return err;
	}

	err = write_register(kRegisterMux, mux_register(g_config.channel));
	if (err != 0) {
		return err;
	}

	err = write_register(kRegisterAdcon, static_cast<uint8_t>(0x20U | pga_bits(g_config.pga)));
	if (err != 0) {
		return err;
	}

	err = write_register(kRegisterDrate, data_rate_register(g_config.data_rate_sps));
	if (err != 0) {
		return err;
	}

	err = write_command(kCommandSelfcal);
	if (err != 0) {
		return err;
	}

	err = wait_drdy(kInitTimeoutMs);
	if (err != 0) {
		return err;
	}

	err = read_register(kRegisterStatus, &status_reg);
	if (err != 0) {
		return err;
	}

	err = read_register(kRegisterMux, &mux_reg);
	if (err != 0) {
		return err;
	}

	err = read_register(kRegisterAdcon, &adcon_reg);
	if (err != 0) {
		return err;
	}

	err = read_register(kRegisterDrate, &drate_reg);
	if (err != 0) {
		return err;
	}

	err = wait_drdy(kInitTimeoutMs);
	if (err != 0) {
		return err;
	}

	err = write_command(kCommandRdatac);
	if (err != 0) {
		return err;
	}

	printk("ADS1256 regs STATUS=0x%02x MUX=0x%02x ADCON=0x%02x DRATE=0x%02x\n",
	       status_reg, mux_reg, adcon_reg, drate_reg);
	return 0;
}

int Ads1256::readSample(int32_t &sample)
{
	const uint8_t tx_data[3] = {0xFFU, 0xFFU, 0xFFU};
	uint8_t rx_data[3] = {};
	int err;

	err = wait_drdy((1000U / g_config.data_rate_sps) + 150U);
	if (err != 0) {
		return err;
	}

	err = spi_transceive_with_config(&kSpi.config, tx_data, rx_data, sizeof(tx_data));
	if (err != 0) {
		return err;
	}

	sample = sign_extend_24(rx_data);
	return 0;
}

Ads1256Config Ads1256::config() const
{
	return g_config;
}

const char *Ads1256::channelName() const
{
	switch (g_config.channel) {
	case 0U:
		return "AIN0";
	case 1U:
		return "AIN1";
	case 2U:
		return "AIN2";
	case 3U:
		return "AIN3";
	case 4U:
		return "AIN4";
	case 5U:
		return "AIN5";
	case 6U:
		return "AIN6";
	case 7U:
		return "AIN7";
	default:
		return "AIN?";
	}
}

} // namespace drivers
