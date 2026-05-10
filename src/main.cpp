#include "app/app_controller.hpp"

int main(void)
{
	app::Controller::instance().init();
	app::Controller::instance().run();
	return 0;
}
