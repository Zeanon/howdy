#pragma once

#include "rubberstamps/stamp.hh"

namespace howdy {

// Wait for the user to press a hotkey to abort or to confirm
class HotkeyStamp : public Stamp {
  public:
	HotkeyStamp();

	bool run(const StampContext& context, const StampRule& rule) override;
};

} // namespace howdy
