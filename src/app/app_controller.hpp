#ifndef PORTABLE_EEG_READER_APP_APP_CONTROLLER_HPP_
#define PORTABLE_EEG_READER_APP_APP_CONTROLLER_HPP_

#include "app/app_events.hpp"
#include "app/app_state.hpp"
#include "services/acquisition_service.hpp"
#include "services/ble_service.hpp"
#include "services/signal_processing_service.hpp"

namespace app {

class Controller {
public:
	static Controller &instance();

	int init();
	void run();

private:
	Controller() = default;

	void handleEvent(const Event &event);
	void transitionTo(AppState new_state);

	AppState state_ = AppState::Boot;
	bool adc_ready_ = false;
	bool battery_ready_ = false;
	bool charging_ = false;
	bool ui_ready_ = false;
	services::AcquisitionService acquisition_;
	services::SignalProcessingService signal_processing_;
	services::BleService ble_;
};

} // namespace app

#endif
