#include "rubberstamps/stamp.hh"

#include <algorithm>
#include <chrono>
#include <thread>

namespace howdy {

void cancelable_sleep(const StampContext& context, std::chrono::milliseconds duration) {
	using Clock = std::chrono::steady_clock;
	using std::chrono::milliseconds;

	constexpr milliseconds SLICE{10};
	const auto end = Clock::now() + duration;
	while (Clock::now() < end) {
		check_cancelled(context);
		std::this_thread::sleep_for(std::min<milliseconds>(SLICE, std::chrono::duration_cast<milliseconds>(end - Clock::now())));
	}
	check_cancelled(context);
}

void check_cancelled(const StampContext& context) {
	if (context.cancel.load()) {
		throw StampCancelled();
	}
	if (context.keep_alive) {
		context.keep_alive();
	}
}

} // namespace howdy
