#include "services/ble_service.hpp"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ble_service, LOG_LEVEL_INF);

namespace services {
namespace {

constexpr size_t kBleTxBufferSize = 128U;
constexpr uint16_t kBleChunkSize = 20U;
constexpr uint32_t kPairingWindowMs = 30000U;
constexpr char kStatusOff[] = "OFF";
constexpr char kStatusPairing[] = "PAIR";
constexpr char kStatusOn[] = "ON";

atomic_t g_notifications_enabled = ATOMIC_INIT(0);
atomic_t g_metadata_pending = ATOMIC_INIT(1);
atomic_t g_status = ATOMIC_INIT(static_cast<atomic_val_t>(BleStatus::Off));
atomic_t g_pairing_requested = ATOMIC_INIT(0);
atomic_t g_secured = ATOMIC_INIT(0);
atomic_t g_has_bond = ATOMIC_INIT(0);
atomic_t g_paired_session = ATOMIC_INIT(0);
atomic_t g_pairing_deadline_ms = ATOMIC_INIT(0);
struct bt_conn *g_current_conn;
drivers::Ads1256Config g_adc_config = {};

const struct bt_data kAdvertisingData[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		(sizeof(CONFIG_BT_DEVICE_NAME) - 1U)),
};

const struct bt_data kScanResponseData[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

const char *status_name(BleStatus status)
{
	switch (status) {
	case BleStatus::On:
		return kStatusOn;
	case BleStatus::Pairing:
		return kStatusPairing;
	case BleStatus::Off:
	default:
		return kStatusOff;
	}
}

void publish_status(BleStatus status)
{
	atomic_set(&g_status, static_cast<atomic_val_t>(status));
	printk("BLE status: %s\n", status_name(status));
}

int start_advertising()
{
	const int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, kAdvertisingData,
					 ARRAY_SIZE(kAdvertisingData), kScanResponseData,
					 ARRAY_SIZE(kScanResponseData));

	if (err == -EALREADY) {
		return 0;
	}

	return err;
}

int stop_advertising()
{
	const int err = bt_le_adv_stop();

	if ((err == 0) || (err == -EALREADY)) {
		return 0;
	}

	return err;
}

void restore_bonded_advertising()
{
	int err;

	if (atomic_get(&g_has_bond) == 0) {
		(void)stop_advertising();
		publish_status(BleStatus::Off);
		return;
	}

	err = start_advertising();
	if (err != 0) {
		printk("BLE advertising restore failed: %d\n", err);
		publish_status(BleStatus::Off);
		return;
	}

	publish_status(BleStatus::On);
}

void stop_pairing_mode(bool keep_ble_on)
{
	atomic_set(&g_pairing_requested, 0);
	atomic_set(&g_pairing_deadline_ms, 0);
	bt_set_bondable(false);

	if (keep_ble_on) {
		publish_status(BleStatus::On);
		return;
	}

	restore_bonded_advertising();
}

int ble_send_chunked(const char *text)
{
	size_t remaining;
	const char *cursor;

	if ((text == nullptr) || (atomic_get(&g_notifications_enabled) == 0) ||
	    (atomic_get(&g_secured) == 0)) {
		return 0;
	}

	cursor = text;
	remaining = strlen(text);

	while (remaining > 0U) {
		const uint16_t chunk_len =
			(remaining > kBleChunkSize) ? kBleChunkSize : static_cast<uint16_t>(remaining);
		int err;

		for (uint8_t attempt = 0U;; ++attempt) {
			err = bt_nus_send(nullptr, cursor, chunk_len);
			if ((err == 0) || (err == -ENOTCONN)) {
				break;
			}

			if ((err != -ENOMEM) && (err != -EAGAIN)) {
				printk("BLE TX error: %d\n", err);
				return err;
			}

			if (attempt >= 9U) {
				return err;
			}

			k_msleep(4);
		}

		if (err != 0) {
			return 0;
		}

		cursor += chunk_len;
		remaining -= chunk_len;
	}

	return 0;
}

void send_adc_csv_header()
{
	(void)ble_send_chunked("ADC_RAW_HEADER,uptime_ms,adc0\n");
}

void send_adc_csv_info()
{
	char line[kBleTxBufferSize];

	(void)snprintk(line, sizeof(line),
		       "ADC_RAW_INFO,adc0,channel_id,%u,resolution,24,min_raw,-8388608,"
		       "max_raw,8388607,gain,%u,data_rate,%u\n",
		       static_cast<unsigned int>(g_adc_config.channel),
		       static_cast<unsigned int>(g_adc_config.pga),
		       static_cast<unsigned int>(g_adc_config.data_rate_sps));
	(void)ble_send_chunked(line);
}

void send_metadata_if_needed()
{
	if (atomic_cas(&g_metadata_pending, 1, 0)) {
		send_adc_csv_info();
		send_adc_csv_header();
	}
}

void nus_notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);

	atomic_set(&g_notifications_enabled, enabled ? 1 : 0);
	atomic_set(&g_metadata_pending, enabled ? 1 : 0);

	if (enabled && (atomic_get(&g_secured) != 0)) {
		publish_status(BleStatus::On);
	}
}

static struct bt_nus_cb g_nus_callbacks = {
	.notif_enabled = nus_notif_enabled,
};

void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U) {
		printk("BLE connect failed: %u\n", err);
		return;
	}

	if (g_current_conn != nullptr) {
		bt_conn_unref(g_current_conn);
	}

	g_current_conn = bt_conn_ref(conn);
	atomic_set(&g_secured, 0);
	atomic_set(&g_metadata_pending, 1);
	printk("BLE connected\n");

	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if ((err != 0) && (err != -EALREADY)) {
		printk("BLE security request failed: %d\n", err);
	}

	if (atomic_get(&g_pairing_requested) != 0) {
		publish_status(BleStatus::Pairing);
	} else if (atomic_get(&g_paired_session) != 0) {
		publish_status(BleStatus::On);
	}
}

void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	int err;

	if (g_current_conn == conn) {
		bt_conn_unref(g_current_conn);
		g_current_conn = nullptr;
	}

	atomic_set(&g_notifications_enabled, 0);
	atomic_set(&g_secured, 0);
	atomic_set(&g_metadata_pending, 1);
	printk("BLE disconnected: %u\n", reason);

	if (atomic_get(&g_pairing_requested) != 0) {
		publish_status(BleStatus::Pairing);
		err = start_advertising();
		if (err != 0) {
			printk("BLE advertising restart failed: %d\n", err);
			stop_pairing_mode(false);
		}
		return;
	}

	if ((atomic_get(&g_paired_session) != 0) || (atomic_get(&g_has_bond) != 0)) {
		restore_bonded_advertising();
		return;
	}

	publish_status(BleStatus::Off);
}

void on_security_changed(struct bt_conn *conn, bt_security_t level,
			 enum bt_security_err err)
{
	ARG_UNUSED(conn);

	if ((err == BT_SECURITY_ERR_SUCCESS) && (level >= BT_SECURITY_L2)) {
		atomic_set(&g_secured, 1);
		atomic_set(&g_has_bond, 1);
		atomic_set(&g_paired_session, 1);
		atomic_set(&g_metadata_pending, 1);
		stop_pairing_mode(true);
		return;
	}

	if (err != BT_SECURITY_ERR_SUCCESS) {
		printk("BLE security failed: %u\n", err);
		atomic_set(&g_secured, 0);
		if (atomic_get(&g_pairing_requested) != 0) {
			publish_status(BleStatus::Pairing);
		} else if (atomic_get(&g_paired_session) == 0) {
			publish_status(BleStatus::Off);
		}
	}
}

BT_CONN_CB_DEFINE(g_connection_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security_changed,
};

enum bt_security_err pairing_accept(struct bt_conn *conn,
					    const struct bt_conn_pairing_feat *const feat)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(feat);

	return (atomic_get(&g_pairing_requested) != 0) ? BT_SECURITY_ERR_SUCCESS :
						       BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

void pairing_confirm(struct bt_conn *conn)
{
	if (atomic_get(&g_pairing_requested) != 0) {
		(void)bt_conn_auth_pairing_confirm(conn);
	} else {
		(void)bt_conn_auth_cancel(conn);
	}
}

void auth_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	if (atomic_get(&g_pairing_requested) != 0) {
		publish_status(BleStatus::Pairing);
	}
}

void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);

	printk("BLE pairing complete%s\n", bonded ? " and bonded" : "");
}

void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);

	printk("BLE pairing failed: %u\n", reason);
	if (atomic_get(&g_pairing_requested) != 0) {
		publish_status(BleStatus::Pairing);
	}
}

const struct bt_conn_auth_cb g_auth_callbacks = {
	.pairing_accept = pairing_accept,
	.cancel = auth_cancel,
	.pairing_confirm = pairing_confirm,
};

struct bt_conn_auth_info_cb g_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

void bond_iter_cb(const struct bt_bond_info *info, void *user_data)
{
	bool *found = static_cast<bool *>(user_data);

	ARG_UNUSED(info);
	*found = true;
}

bool load_bond_state()
{
	bool found = false;

	bt_foreach_bond(BT_ID_DEFAULT, bond_iter_cb, &found);
	atomic_set(&g_has_bond, found ? 1 : 0);
	atomic_set(&g_paired_session, found ? 1 : 0);
	return found;
}

} // namespace

int BleService::init(const drivers::Ads1256Config &adc_config)
{
	int err;
	bool has_bond = false;

	g_adc_config = adc_config;

	err = bt_nus_cb_register(&g_nus_callbacks, nullptr);
	if (err != 0) {
		return err;
	}

	err = bt_conn_auth_cb_register(&g_auth_callbacks);
	if (err != 0) {
		return err;
	}

	err = bt_conn_auth_info_cb_register(&g_auth_info_callbacks);
	if (err != 0) {
		return err;
	}

	err = bt_enable(nullptr);
	if (err != 0) {
		publish_status(BleStatus::Off);
		return err;
	}

#if defined(CONFIG_BT_SETTINGS)
	settings_load();
#endif

	bt_set_bondable(false);
	has_bond = load_bond_state();
	if (has_bond) {
		restore_bonded_advertising();
	} else {
		publish_status(BleStatus::Off);
	}
	printk("BLE ready as %s\n", CONFIG_BT_DEVICE_NAME);
	return 0;
}

int BleService::requestPairing()
{
	int err;

	atomic_set(&g_pairing_requested, 1);
	atomic_set(&g_pairing_deadline_ms, k_uptime_get_32() + kPairingWindowMs);
	atomic_set(&g_paired_session, 0);
	atomic_set(&g_secured, 0);
	atomic_set(&g_notifications_enabled, 0);
	atomic_set(&g_metadata_pending, 1);
	bt_set_bondable(true);
	(void)bt_unpair(BT_ID_DEFAULT, nullptr);
	publish_status(BleStatus::Pairing);

	if (g_current_conn != nullptr) {
		err = bt_conn_disconnect(g_current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if ((err != 0) && (err != -ENOTCONN)) {
			printk("BLE disconnect before pairing failed: %d\n", err);
		}
	}

	err = start_advertising();
	if (err != 0) {
		stop_pairing_mode(false);
		return err;
	}

	printk("BLE pairing window opened for %" PRIu32 " ms\n", kPairingWindowMs);
	return 0;
}

BleStatus BleService::status() const
{
	return static_cast<BleStatus>(atomic_get(&g_status));
}

bool BleService::pairingActive() const
{
	return atomic_get(&g_pairing_requested) != 0;
}

uint32_t BleService::pairingRemainingMs(uint32_t now_ms) const
{
	const uint32_t deadline = static_cast<uint32_t>(atomic_get(&g_pairing_deadline_ms));

	if ((atomic_get(&g_pairing_requested) == 0) || (deadline == 0U)) {
		return 0U;
	}

	if (static_cast<int32_t>(deadline - now_ms) <= 0) {
		return 0U;
	}

	return deadline - now_ms;
}

void BleService::tick(uint32_t now_ms)
{
	const uint32_t deadline = static_cast<uint32_t>(atomic_get(&g_pairing_deadline_ms));

	if ((atomic_get(&g_pairing_requested) == 0) || (deadline == 0U)) {
		return;
	}

	if (static_cast<int32_t>(deadline - now_ms) > 0) {
		return;
	}

	printk("BLE pairing window expired\n");
	if (g_current_conn != nullptr) {
		(void)bt_conn_disconnect(g_current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}

	stop_pairing_mode(false);
}

int BleService::sendAdcCsvRow(uint32_t uptime_ms, const headband::SampleBuffer &samples)
{
	char line[kBleTxBufferSize];

	if ((atomic_get(&g_notifications_enabled) == 0) || (atomic_get(&g_secured) == 0)) {
		return 0;
	}

	send_metadata_if_needed();

	(void)snprintk(line, sizeof(line), "ADC_RAW,%" PRIu32 ",%" PRId32 "\n", uptime_ms,
		       samples.values[0]);
	return ble_send_chunked(line);
}

} // namespace services
