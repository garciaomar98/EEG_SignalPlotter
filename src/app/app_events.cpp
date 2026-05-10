#include "app/app_events.hpp"

#include <zephyr/kernel.h>

namespace app {
namespace {

K_MSGQ_DEFINE(g_event_queue, sizeof(Event), 16, 4);

} // namespace

int publish(const Event &event)
{
	return k_msgq_put(&g_event_queue, &event, K_NO_WAIT);
}

int wait(Event &event, k_timeout_t timeout)
{
	return k_msgq_get(&g_event_queue, &event, timeout);
}

} // namespace app
