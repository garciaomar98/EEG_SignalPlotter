#include "ui/ui_events.hpp"

#include <zephyr/kernel.h>

#include "app/app_events.hpp"

namespace ui {

void publish_start_acquisition_requested()
{
	(void)app::publish({
		.type = app::EventType::StartAcquisitionRequested,
		.timestamp_ms = k_uptime_get_32(),
		.samples = {},
		.error = 0,
	});
}

void publish_stop_acquisition_requested()
{
	(void)app::publish({
		.type = app::EventType::StopAcquisitionRequested,
		.timestamp_ms = k_uptime_get_32(),
		.samples = {},
		.error = 0,
	});
}

} // namespace ui
