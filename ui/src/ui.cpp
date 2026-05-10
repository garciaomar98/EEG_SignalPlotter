#include "ui/ui.hpp"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

BUILD_ASSERT(DT_NODE_HAS_STATUS(DISPLAY_NODE, okay),
	     "Board DTS must provide zephyr,display");

namespace {

constexpr uint16_t kChartPointCount = 72U;
constexpr int32_t kDefaultChartMin = -1;
constexpr int32_t kDefaultChartMax = 1;
constexpr int kBatteryLevelUnknown = -1;
constexpr uint16_t kReferenceDisplayWidth = 240U;
constexpr uint16_t kReferenceDisplayHeight = 240U;
constexpr char kDefaultQualityText[] = "Signal quality: --";
constexpr char kPairingHintText[] = "Visible for 30s";
const lv_color_t kScreenTopColor = lv_color_hex(0x081018);
const lv_color_t kScreenBottomColor = lv_color_hex(0x12314c);
const lv_color_t kCardColor = lv_color_hex(0x122033);
const lv_color_t kCardBorderColor = lv_color_hex(0x29425d);
const lv_color_t kPrimaryTextColor = lv_color_hex(0xf2f7ff);
const lv_color_t kSecondaryTextColor = lv_color_hex(0x9db4c8);
const lv_color_t kBleConnectedColor = lv_color_hex(0x4ce08a);
const lv_color_t kBlePairingColor = lv_color_hex(0xffd166);
const lv_color_t kBleDisconnectedColor = lv_color_hex(0xff5a5f);
const lv_color_t kPairingTrackColor = lv_color_hex(0x26384d);
const lv_color_t kAdcLiveColor = lv_color_hex(0x1f8a70);
const lv_color_t kAdcOfflineColor = lv_color_hex(0xbf7b28);
const lv_color_t kSeriesColors[headband::kMaxPlotChannels] = {
	lv_color_hex(0xff6b6b),
};

const struct device *const kDisplay = DEVICE_DT_GET(DISPLAY_NODE);

struct UiContext {
	bool initialized;
	bool adc_ready;
	bool charging;
	bool pairing_active;
	ui::BleStatus ble_status;
	int battery_percent;
	uint32_t channel_mask;
	uint32_t pairing_remaining_ms;
	uint32_t pairing_timeout_ms;
	uint16_t display_width;
	uint16_t display_height;
	headband::SampleBuffer latest_samples;
	char quality_text[32];
	lv_obj_t *root_screen;
	lv_obj_t *status_bar;
	lv_obj_t *battery_label;
	lv_obj_t *ble_label;
	lv_obj_t *chart;
	lv_obj_t *chart_card;
	lv_obj_t *legend_card;
	lv_obj_t *quality_chip;
	lv_obj_t *quality_label;
	lv_obj_t *charging_overlay;
	lv_obj_t *charging_icon;
	lv_obj_t *charging_text;
	lv_obj_t *pairing_overlay;
	lv_obj_t *pairing_title;
	lv_obj_t *pairing_arc;
	lv_obj_t *pairing_seconds;
	lv_obj_t *pairing_hint;
	lv_obj_t *value_labels[headband::kMaxPlotChannels];
	lv_chart_series_t *series[headband::kMaxPlotChannels];
};

UiContext g_ui = {};

int16_t scale_x(uint16_t value)
{
	if (g_ui.display_width == 0U) {
		return static_cast<int16_t>(value);
	}

	return static_cast<int16_t>((static_cast<uint32_t>(value) * g_ui.display_width) /
				    kReferenceDisplayWidth);
}

int16_t scale_y(uint16_t value)
{
	if (g_ui.display_height == 0U) {
		return static_cast<int16_t>(value);
	}

	return static_cast<int16_t>((static_cast<uint32_t>(value) * g_ui.display_height) /
				    kReferenceDisplayHeight);
}

bool channel_enabled(uint8_t channel)
{
	return (g_ui.channel_mask & (1U << channel)) != 0U;
}

void reset_dashboard_handles_locked()
{
	g_ui.status_bar = nullptr;
	g_ui.battery_label = nullptr;
	g_ui.ble_label = nullptr;
	g_ui.chart = nullptr;
	g_ui.chart_card = nullptr;
	g_ui.legend_card = nullptr;
	g_ui.quality_chip = nullptr;
	g_ui.quality_label = nullptr;
	g_ui.charging_overlay = nullptr;
	g_ui.charging_icon = nullptr;
	g_ui.charging_text = nullptr;
	g_ui.pairing_overlay = nullptr;
	g_ui.pairing_title = nullptr;
	g_ui.pairing_arc = nullptr;
	g_ui.pairing_seconds = nullptr;
	g_ui.pairing_hint = nullptr;

	for (uint8_t channel = 0U; channel < headband::kMaxPlotChannels; ++channel) {
		g_ui.value_labels[channel] = nullptr;
		g_ui.series[channel] = nullptr;
	}
}

void prepare_root_screen_locked()
{
	g_ui.root_screen = lv_screen_active();
	lv_obj_clean(g_ui.root_screen);
	lv_obj_set_style_bg_color(g_ui.root_screen, kScreenTopColor, 0);
	lv_obj_set_style_bg_grad_color(g_ui.root_screen, kScreenBottomColor, 0);
	lv_obj_set_style_bg_grad_dir(g_ui.root_screen, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_pad_all(g_ui.root_screen, 0, 0);
	lv_obj_set_scrollbar_mode(g_ui.root_screen, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t *create_card(lv_obj_t *parent, int16_t x, int16_t y, int16_t width, int16_t height)
{
	lv_obj_t *card = lv_obj_create(parent);

	lv_obj_remove_style_all(card);
	lv_obj_set_pos(card, x, y);
	lv_obj_set_size(card, width, height);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_set_style_bg_color(card, kCardColor, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_color(card, kCardBorderColor, 0);
	lv_obj_set_style_pad_all(card, 0, 0);

	return card;
}

const char *battery_symbol_for_level(int percent)
{
	if (percent < 0) {
		return LV_SYMBOL_BATTERY_EMPTY;
	}

	if (percent >= 90) {
		return LV_SYMBOL_BATTERY_FULL;
	}

	if (percent >= 65) {
		return LV_SYMBOL_BATTERY_3;
	}

	if (percent >= 40) {
		return LV_SYMBOL_BATTERY_2;
	}

	if (percent >= 15) {
		return LV_SYMBOL_BATTERY_1;
	}

	return LV_SYMBOL_BATTERY_EMPTY;
}

void format_compact_sample(int32_t sample, char *buffer, size_t buffer_size)
{
	int64_t absolute_value = sample;
	const char sign = (sample < 0) ? '-' : '+';

	if (absolute_value < 0) {
		absolute_value = -absolute_value;
	}

	if (absolute_value >= 1000000LL) {
		(void)snprintf(buffer, buffer_size, "%c%" PRId64 ".%01" PRId64 "M", sign,
			       absolute_value / 1000000LL,
			       (absolute_value % 1000000LL) / 100000LL);
		return;
	}

	if (absolute_value >= 1000LL) {
		(void)snprintf(buffer, buffer_size, "%c%" PRId64 ".%01" PRId64 "k", sign,
			       absolute_value / 1000LL,
			       (absolute_value % 1000LL) / 100LL);
		return;
	}

	(void)snprintf(buffer, buffer_size, "%+" PRId32, sample);
}

void set_battery_level_locked(int percent)
{
	char text[24];

	g_ui.battery_percent = percent;

	if (g_ui.battery_label == nullptr) {
		return;
	}

	if (percent < 0) {
		lv_obj_set_style_text_color(g_ui.battery_label, kSecondaryTextColor, 0);
		(void)snprintf(text, sizeof(text), "%s --", battery_symbol_for_level(percent));
	} else {
		lv_obj_set_style_text_color(g_ui.battery_label, kPrimaryTextColor, 0);
		(void)snprintf(text, sizeof(text), "%s %d%%", battery_symbol_for_level(percent), percent);
	}

	lv_label_set_text(g_ui.battery_label, text);
}

void set_ble_status_locked(ui::BleStatus status)
{
	const char *status_text = "OFF";
	lv_color_t status_color = kBleDisconnectedColor;

	g_ui.ble_status = status;

	if (g_ui.ble_label == nullptr) {
		return;
	}

	switch (status) {
	case ui::BleStatus::On:
		status_text = "ON";
		status_color = kBleConnectedColor;
		break;
	case ui::BleStatus::Pairing:
		status_text = "Pair";
		status_color = kBlePairingColor;
		break;
	case ui::BleStatus::Off:
	default:
		break;
	}

	lv_obj_set_style_text_color(g_ui.ble_label, status_color, 0);
	lv_label_set_text_fmt(g_ui.ble_label, "%s %s", LV_SYMBOL_BLUETOOTH, status_text);
}

void set_signal_quality_locked(const char *text)
{
	const char *next_text = ((text != nullptr) && (text[0] != '\0')) ? text : kDefaultQualityText;

	(void)snprintf(g_ui.quality_text, sizeof(g_ui.quality_text), "%s", next_text);

	if (g_ui.quality_label == nullptr) {
		return;
	}

	lv_label_set_text(g_ui.quality_label, g_ui.quality_text);
}

void set_status_locked(bool adc_ready)
{
	const lv_color_t accent_color = adc_ready ? kAdcLiveColor : kAdcOfflineColor;

	g_ui.adc_ready = adc_ready;

	if (g_ui.status_bar != nullptr) {
		lv_obj_set_style_border_color(g_ui.status_bar, accent_color, 0);
	}

	if (g_ui.quality_chip != nullptr) {
		lv_obj_set_style_border_color(g_ui.quality_chip, accent_color, 0);
	}

	if (g_ui.quality_label != nullptr) {
		lv_obj_set_style_text_color(g_ui.quality_label,
					    adc_ready ? kPrimaryTextColor : kSecondaryTextColor, 0);
	}
}

void update_pairing_overlay_locked()
{
	uint32_t progress_percent = 0U;
	uint32_t seconds_remaining = 0U;

	if ((g_ui.pairing_arc == nullptr) || (g_ui.pairing_seconds == nullptr)) {
		return;
	}

	if ((g_ui.pairing_timeout_ms > 0U) &&
	    (g_ui.pairing_remaining_ms < g_ui.pairing_timeout_ms)) {
		const uint32_t elapsed_ms = g_ui.pairing_timeout_ms - g_ui.pairing_remaining_ms;

		progress_percent =
			(elapsed_ms * 100U + (g_ui.pairing_timeout_ms / 2U)) / g_ui.pairing_timeout_ms;
		if (progress_percent > 100U) {
			progress_percent = 100U;
		}
	}

	if (g_ui.pairing_remaining_ms > 0U) {
		seconds_remaining = (g_ui.pairing_remaining_ms + 999U) / 1000U;
	}

	lv_arc_set_value(g_ui.pairing_arc, static_cast<int32_t>(progress_percent));
	lv_label_set_text_fmt(g_ui.pairing_seconds, "%" PRIu32 "s", seconds_remaining);
}

void refresh_operating_mode_locked()
{
	const bool pairing_visible = g_ui.pairing_active;
	const bool charging_visible = !pairing_visible && g_ui.charging;

	if (g_ui.pairing_overlay != nullptr) {
		if (pairing_visible) {
			lv_obj_clear_flag(g_ui.pairing_overlay, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_foreground(g_ui.pairing_overlay);
		} else {
			lv_obj_add_flag(g_ui.pairing_overlay, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (g_ui.charging_overlay != nullptr) {
		if (charging_visible) {
			lv_obj_clear_flag(g_ui.charging_overlay, LV_OBJ_FLAG_HIDDEN);
			lv_obj_move_foreground(g_ui.charging_overlay);
		} else {
			lv_obj_add_flag(g_ui.charging_overlay, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (g_ui.status_bar != nullptr) {
		if (charging_visible) {
			lv_obj_add_flag(g_ui.status_bar, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_clear_flag(g_ui.status_bar, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (g_ui.chart_card != nullptr) {
		if (charging_visible) {
			lv_obj_add_flag(g_ui.chart_card, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_clear_flag(g_ui.chart_card, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (g_ui.legend_card != nullptr) {
		if (charging_visible) {
			lv_obj_add_flag(g_ui.legend_card, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_clear_flag(g_ui.legend_card, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (g_ui.quality_chip != nullptr) {
		if (charging_visible) {
			lv_obj_add_flag(g_ui.quality_chip, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_clear_flag(g_ui.quality_chip, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

void refresh_chart_range_locked()
{
	bool have_value = false;
	int32_t min_value = 0;
	int32_t max_value = 0;

	if (g_ui.chart == nullptr) {
		return;
	}

	for (uint8_t channel = 0U; channel < headband::kMaxPlotChannels; ++channel) {
		int32_t *points;

		if ((g_ui.series[channel] == nullptr) || !channel_enabled(channel)) {
			continue;
		}

		points = lv_chart_get_series_y_array(g_ui.chart, g_ui.series[channel]);
		if (points == nullptr) {
			continue;
		}

		for (uint16_t index = 0U; index < kChartPointCount; ++index) {
			const int32_t value = points[index];

			if (value == LV_CHART_POINT_NONE) {
				continue;
			}

			if (!have_value) {
				min_value = value;
				max_value = value;
				have_value = true;
				continue;
			}

			if (value < min_value) {
				min_value = value;
			}

			if (value > max_value) {
				max_value = value;
			}
		}
	}

	if (!have_value) {
		lv_chart_set_axis_range(g_ui.chart, LV_CHART_AXIS_PRIMARY_Y, kDefaultChartMin,
					 kDefaultChartMax);
		lv_chart_refresh(g_ui.chart);
		return;
	}

	if (min_value == max_value) {
		const int32_t padding =
			(min_value == 0) ? 1 : (min_value < 0 ? -min_value : min_value) / 8 + 1;

		min_value -= padding;
		max_value += padding;
	} else {
		const int64_t span =
			static_cast<int64_t>(max_value) - static_cast<int64_t>(min_value);
		const int32_t padding = static_cast<int32_t>(span / 8LL) + 1;

		min_value -= padding;
		max_value += padding;
	}

	lv_chart_set_axis_range(g_ui.chart, LV_CHART_AXIS_PRIMARY_Y, min_value, max_value);
	lv_chart_refresh(g_ui.chart);
}

void update_samples_locked(const headband::SampleBuffer &samples)
{
	for (uint8_t channel = 0U; channel < headband::kMaxPlotChannels; ++channel) {
		char compact_value[16];

		g_ui.latest_samples.values[channel] = samples.values[channel];

		if (!channel_enabled(channel)) {
			continue;
		}

		if (g_ui.value_labels[channel] != nullptr) {
			format_compact_sample(samples.values[channel], compact_value, sizeof(compact_value));
			lv_label_set_text_fmt(g_ui.value_labels[channel], "ADC:%s", compact_value);
		}

		if (g_ui.series[channel] != nullptr) {
			lv_chart_set_next_value(g_ui.chart, g_ui.series[channel],
						samples.values[channel]);
		}
	}

	refresh_chart_range_locked();
}

void build_dashboard_screen_locked()
{
	lv_obj_t *chart_card;
	lv_obj_t *legend_card;
	lv_obj_t *value_label;
	const int16_t status_x = scale_x(50U);
	const int16_t status_y = scale_y(14U);
	const int16_t status_width = scale_x(140U);
	const int16_t status_height = scale_y(30U);
	const int16_t chart_card_x = scale_x(18U);
	const int16_t chart_card_y = scale_y(52U);
	const int16_t chart_card_width = scale_x(204U);
	const int16_t chart_card_height = scale_y(112U);
	const int16_t chart_width = scale_x(180U);
	const int16_t chart_height = scale_y(88U);
	const int16_t legend_x = scale_x(18U);
	const int16_t legend_y = scale_y(172U);
	const int16_t legend_width = scale_x(204U);
	const int16_t legend_height = scale_y(34U);
	const int16_t quality_x = scale_x(28U);
	const int16_t quality_y = scale_y(212U);
	const int16_t quality_width = scale_x(184U);
	const int16_t quality_height = scale_y(18U);
	uint8_t visible_index = 0U;

	prepare_root_screen_locked();
	reset_dashboard_handles_locked();

	g_ui.status_bar = create_card(g_ui.root_screen, status_x, status_y, status_width, status_height);
	lv_obj_set_style_radius(g_ui.status_bar, scale_y(15U), 0);
	g_ui.battery_label = lv_label_create(g_ui.status_bar);
	lv_obj_align(g_ui.battery_label, LV_ALIGN_LEFT_MID, scale_x(12U), 0);
	g_ui.ble_label = lv_label_create(g_ui.status_bar);
	lv_obj_align(g_ui.ble_label, LV_ALIGN_RIGHT_MID, -scale_x(12U), 0);

	chart_card = create_card(g_ui.root_screen, chart_card_x, chart_card_y, chart_card_width,
				 chart_card_height);
	g_ui.chart_card = chart_card;
	g_ui.chart = lv_chart_create(chart_card);
	lv_obj_set_size(g_ui.chart, chart_width, chart_height);
	lv_obj_align(g_ui.chart, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_opa(g_ui.chart, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(g_ui.chart, 0, 0);
	lv_obj_set_style_line_width(g_ui.chart, 2, LV_PART_ITEMS);
	lv_obj_set_style_line_color(g_ui.chart, kCardBorderColor, LV_PART_MAIN);
	lv_obj_set_style_pad_left(g_ui.chart, 0, 0);
	lv_obj_set_style_pad_right(g_ui.chart, 0, 0);
	lv_obj_set_style_pad_top(g_ui.chart, 0, 0);
	lv_obj_set_style_pad_bottom(g_ui.chart, 0, 0);
	lv_obj_set_style_size(g_ui.chart, 0, 0, LV_PART_INDICATOR);
	lv_chart_set_type(g_ui.chart, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(g_ui.chart, kChartPointCount);
	lv_chart_set_axis_range(g_ui.chart, LV_CHART_AXIS_PRIMARY_Y, kDefaultChartMin,
				 kDefaultChartMax);
	lv_chart_set_div_line_count(g_ui.chart, 3, 3);
	lv_chart_set_update_mode(g_ui.chart, LV_CHART_UPDATE_MODE_SHIFT);

	legend_card = create_card(g_ui.root_screen, legend_x, legend_y, legend_width, legend_height);
	g_ui.legend_card = legend_card;
	lv_obj_set_style_radius(legend_card, scale_y(14U), 0);
	for (uint8_t channel = 0U; channel < headband::kMaxPlotChannels; ++channel) {
		if (!channel_enabled(channel)) {
			continue;
		}

		g_ui.series[channel] =
			lv_chart_add_series(g_ui.chart, kSeriesColors[channel], LV_CHART_AXIS_PRIMARY_Y);
		lv_chart_set_all_values(g_ui.chart, g_ui.series[channel], 0);

		value_label = lv_label_create(legend_card);
		lv_obj_set_width(value_label, scale_x(188U));
		lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);
		lv_obj_set_style_text_color(value_label, kSeriesColors[channel], 0);
		lv_obj_set_pos(value_label,
			       static_cast<int16_t>(scale_x(8U + ((visible_index % 4U) * 49U))),
			       static_cast<int16_t>(scale_y(5U + ((visible_index / 4U) * 13U))));
		g_ui.value_labels[channel] = value_label;
		++visible_index;
	}

	g_ui.quality_chip = create_card(g_ui.root_screen, quality_x, quality_y, quality_width,
					quality_height);
	lv_obj_set_style_radius(g_ui.quality_chip, scale_y(9U), 0);
	g_ui.quality_label = lv_label_create(g_ui.quality_chip);
	lv_obj_set_width(g_ui.quality_label, scale_x(168U));
	lv_label_set_long_mode(g_ui.quality_label, LV_LABEL_LONG_CLIP);
	lv_obj_set_style_text_align(g_ui.quality_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(g_ui.quality_label, g_ui.quality_text);
	lv_obj_center(g_ui.quality_label);

	g_ui.charging_overlay = lv_obj_create(g_ui.root_screen);
	lv_obj_remove_style_all(g_ui.charging_overlay);
	lv_obj_set_size(g_ui.charging_overlay, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(g_ui.charging_overlay, kScreenTopColor, 0);
	lv_obj_set_style_bg_grad_color(g_ui.charging_overlay, kScreenBottomColor, 0);
	lv_obj_set_style_bg_grad_dir(g_ui.charging_overlay, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_pad_all(g_ui.charging_overlay, 0, 0);
	lv_obj_add_flag(g_ui.charging_overlay, LV_OBJ_FLAG_HIDDEN);

	g_ui.charging_icon = lv_label_create(g_ui.charging_overlay);
	lv_obj_set_style_text_font(g_ui.charging_icon, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(g_ui.charging_icon, kPrimaryTextColor, 0);
	lv_label_set_text_fmt(g_ui.charging_icon, "%s %s", LV_SYMBOL_CHARGE, LV_SYMBOL_BATTERY_FULL);
	lv_obj_align(g_ui.charging_icon, LV_ALIGN_CENTER, 0, -scale_y(16U));

	g_ui.charging_text = lv_label_create(g_ui.charging_overlay);
	lv_obj_set_style_text_font(g_ui.charging_text, &lv_font_montserrat_22, 0);
	lv_obj_set_style_text_color(g_ui.charging_text, kPrimaryTextColor, 0);
	lv_label_set_text(g_ui.charging_text, "Charging");
	lv_obj_align_to(g_ui.charging_text, g_ui.charging_icon, LV_ALIGN_OUT_BOTTOM_MID, 0,
			scale_y(10U));

	g_ui.pairing_overlay = lv_obj_create(g_ui.root_screen);
	lv_obj_remove_style_all(g_ui.pairing_overlay);
	lv_obj_set_size(g_ui.pairing_overlay, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(g_ui.pairing_overlay, kScreenTopColor, 0);
	lv_obj_set_style_bg_grad_color(g_ui.pairing_overlay, kScreenBottomColor, 0);
	lv_obj_set_style_bg_grad_dir(g_ui.pairing_overlay, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_pad_all(g_ui.pairing_overlay, 0, 0);
	lv_obj_add_flag(g_ui.pairing_overlay, LV_OBJ_FLAG_HIDDEN);

	g_ui.pairing_title = lv_label_create(g_ui.pairing_overlay);
	lv_obj_set_style_text_font(g_ui.pairing_title, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(g_ui.pairing_title, kPrimaryTextColor, 0);
	lv_label_set_text(g_ui.pairing_title, "Pairing");
	lv_obj_align(g_ui.pairing_title, LV_ALIGN_TOP_MID, 0, scale_y(14U));

	g_ui.pairing_arc = lv_arc_create(g_ui.pairing_overlay);
	lv_obj_set_size(g_ui.pairing_arc, scale_y(92U), scale_y(92U));
	lv_obj_align(g_ui.pairing_arc, LV_ALIGN_CENTER, 0, scale_y(10U));
	lv_obj_set_style_arc_width(g_ui.pairing_arc, scale_y(10U), LV_PART_MAIN);
	lv_obj_set_style_arc_width(g_ui.pairing_arc, scale_y(10U), LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(g_ui.pairing_arc, kPairingTrackColor, LV_PART_MAIN);
	lv_obj_set_style_arc_color(g_ui.pairing_arc, kBlePairingColor, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(g_ui.pairing_arc, true, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(g_ui.pairing_arc, LV_OPA_TRANSP, 0);
	lv_obj_remove_style(g_ui.pairing_arc, nullptr, LV_PART_KNOB);
	lv_obj_clear_flag(g_ui.pairing_arc, LV_OBJ_FLAG_CLICKABLE);
	lv_arc_set_rotation(g_ui.pairing_arc, 270);
	lv_arc_set_bg_angles(g_ui.pairing_arc, 0, 360);
	lv_arc_set_range(g_ui.pairing_arc, 0, 100);
	lv_arc_set_value(g_ui.pairing_arc, 0);

	g_ui.pairing_seconds = lv_label_create(g_ui.pairing_overlay);
	lv_obj_set_style_text_font(g_ui.pairing_seconds, &lv_font_montserrat_22, 0);
	lv_obj_set_style_text_color(g_ui.pairing_seconds, kPrimaryTextColor, 0);
	lv_label_set_text(g_ui.pairing_seconds, "30s");
	lv_obj_align_to(g_ui.pairing_seconds, g_ui.pairing_arc, LV_ALIGN_CENTER, 0, 0);

	g_ui.pairing_hint = lv_label_create(g_ui.pairing_overlay);
	lv_obj_set_style_text_font(g_ui.pairing_hint, &lv_font_montserrat_22, 0);
	lv_obj_set_style_text_color(g_ui.pairing_hint, kSecondaryTextColor, 0);
	lv_label_set_text(g_ui.pairing_hint, kPairingHintText);
	lv_obj_align(g_ui.pairing_hint, LV_ALIGN_BOTTOM_MID, 0, -scale_y(18U));

	set_battery_level_locked(g_ui.battery_percent);
	set_ble_status_locked(g_ui.ble_status);
	set_status_locked(g_ui.adc_ready);
	update_pairing_overlay_locked();
	update_samples_locked(g_ui.latest_samples);
	refresh_operating_mode_locked();
}

} // namespace

namespace ui {

int init(uint32_t channel_mask)
{
	int err;
	struct display_capabilities capabilities;

	if (!device_is_ready(kDisplay)) {
		return -ENODEV;
	}

	err = lvgl_init();
	if (err != 0) {
		return err;
	}

	lvgl_lock();
	display_get_capabilities(kDisplay, &capabilities);
	g_ui.channel_mask = channel_mask;
	g_ui.display_width = capabilities.x_resolution;
	g_ui.display_height = capabilities.y_resolution;
	headband::clear_samples(&g_ui.latest_samples);
	g_ui.adc_ready = false;
	g_ui.charging = false;
	g_ui.pairing_active = false;
	g_ui.ble_status = BleStatus::Off;
	g_ui.battery_percent = kBatteryLevelUnknown;
	g_ui.pairing_remaining_ms = 0U;
	g_ui.pairing_timeout_ms = 30000U;
	(void)snprintf(g_ui.quality_text, sizeof(g_ui.quality_text), "%s", kDefaultQualityText);
	build_dashboard_screen_locked();
	lv_timer_handler();
	lvgl_unlock();

	err = display_blanking_off(kDisplay);
	if (err != 0) {
		return err;
	}

	g_ui.initialized = true;
	return 0;
}

void set_adc_ready(bool ready)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	set_status_locked(ready);
	lvgl_unlock();
}

void set_battery_level(int percent)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	set_battery_level_locked(percent);
	lvgl_unlock();
}

void set_charging(bool charging)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	g_ui.charging = charging;
	refresh_operating_mode_locked();
	lvgl_unlock();
}

void set_ble_status(BleStatus status)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	set_ble_status_locked(status);
	lvgl_unlock();
}

void set_ble_connected(bool connected)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	set_ble_status_locked(connected ? BleStatus::On : BleStatus::Off);
	lvgl_unlock();
}

void set_pairing_progress(bool active, uint32_t remaining_ms, uint32_t timeout_ms)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	g_ui.pairing_active = active;
	g_ui.pairing_remaining_ms = remaining_ms;
	g_ui.pairing_timeout_ms = (timeout_ms == 0U) ? 1U : timeout_ms;
	update_pairing_overlay_locked();
	refresh_operating_mode_locked();
	lvgl_unlock();
}

void set_signal_quality(const char *text)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	set_signal_quality_locked(text);
	lvgl_unlock();
}

void update_samples(const headband::SampleBuffer &samples)
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	update_samples_locked(samples);
	lvgl_unlock();
}

void tick()
{
	if (!g_ui.initialized) {
		return;
	}

	lvgl_lock();
	(void)lv_timer_handler();
	lvgl_unlock();
}

} // namespace ui
