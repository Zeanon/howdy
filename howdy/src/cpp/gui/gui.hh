#pragma once

namespace gtkui {

// The graphical interface of Howdy: the configuration window, or the setup
// wizard if there are no models yet or if it is forced. Blocks until it is
// closed and returns the exit code.
int run_gui(bool force_onboarding);

} // namespace gtkui
