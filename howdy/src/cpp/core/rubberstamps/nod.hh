#pragma once

#include "rubberstamps/stamp.hh"

namespace howdy {

// Track the nose of the user to see if they nod yes or shake their head no
class NodStamp : public Stamp {
  public:
	NodStamp();

	bool run(const StampContext& context, const StampRule& rule) override;
};

} // namespace howdy
