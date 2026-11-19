// howdy gui: the configuration window, also started as howdy-gtk

#include "cli.hh"

#ifdef HOWDY_WITH_GUI
#include "gui.hh"
#endif

namespace howdy::cli {

int run_gui(const Options& options) {
#ifdef HOWDY_WITH_GUI
	return gtkui::run_gui(options.force_onboarding);
#else
	(void)options;
	throw CliError("howdy was built without the graphical interface");
#endif
}

} // namespace howdy::cli
