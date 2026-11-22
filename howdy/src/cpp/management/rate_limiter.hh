#pragma once

#include <algorithm>
#include <chrono>
#include <map>

#include <sys/types.h>

namespace howdy {

// A token bucket for each key (the user id of a caller). A bucket holds up to `burst` tokens and
// is filled `rate_per_second` tokens a second, every use takes one. The clock is an argument so a
// test does not have to wait.
class RateLimiter {
  public:
	using Clock = std::chrono::steady_clock;

	RateLimiter(double rate_per_second, double burst) : rate_(rate_per_second), burst_(burst) {
	}

	// Whether the key may do it now. A refused try takes nothing.
	[[nodiscard]] bool allow(uid_t key, Clock::time_point now) {
		const auto [found, inserted] = buckets_.try_emplace(key, Bucket{burst_, now});
		Bucket& bucket = found->second;

		const double elapsed = std::max(std::chrono::duration<double>(now - bucket.last).count(), 0.0);
		bucket.tokens = std::min(burst_, bucket.tokens + (elapsed * rate_));
		bucket.last = now;
		if (bucket.tokens < 1.0) {
			return false;
		}
		bucket.tokens -= 1.0;
		return true;
	}

  private:
	struct Bucket {
		double tokens;
		Clock::time_point last;
	};

	double rate_;
	double burst_;
	std::map<uid_t, Bucket> buckets_;
};

} // namespace howdy
