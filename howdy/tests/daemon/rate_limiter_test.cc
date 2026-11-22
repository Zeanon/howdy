// Test of the token bucket that limits how often a user may call Compare (rate_limiter.hh):
// the burst, the refill, separate keys, and that a refused try takes nothing.
#include <chrono>
#include <cstdio>
#include <string>

#include "management/rate_limiter.hh"

namespace {

bool failed = false;

void check(const std::string& name, bool ok) {
	failed = failed || !ok;
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name.c_str());
}

using Clock = howdy::RateLimiter::Clock;

int count_allowed(howdy::RateLimiter& limiter, uid_t key, Clock::time_point now, int tries) {
	int allowed = 0;
	for (int i = 0; i < tries; ++i) {
		allowed += limiter.allow(key, now) ? 1 : 0;
	}
	return allowed;
}

} // namespace

int main() {
	const auto start = Clock::now();
	const auto after = [&](double seconds) { return start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)); };

	// 30 a second, a burst of 60
	howdy::RateLimiter limiter(30, 60);
	check("the burst is allowed at once", count_allowed(limiter, 1000, start, 100) == 60);
	check("then it is refused", !limiter.allow(1000, start));
	check("a refused try takes nothing: a thirtieth of a second brings one", limiter.allow(1000, after(1.0 / 30 + 0.001)));
	check("and only one", !limiter.allow(1000, after(1.0 / 30 + 0.002)));

	check("after a second thirty are allowed", count_allowed(limiter, 1000, after(1.1), 100) >= 30);
	check("and not much more", count_allowed(limiter, 1000, after(1.1), 100) == 0);

	check("another user has a bucket of its own", count_allowed(limiter, 1001, start, 100) == 60);

	// A long pause brings the burst, not more
	howdy::RateLimiter rested(30, 60);
	check("a first use", rested.allow(7, start));
	check("after a long pause the burst is allowed", count_allowed(rested, 7, after(3600), 100) == 60);

	// The clock going back adds nothing
	howdy::RateLimiter backwards(30, 2);
	check("the burst of two", count_allowed(backwards, 1, after(10), 5) == 2);
	check("a time before adds nothing", !backwards.allow(1, after(5)));

	return failed ? 1 : 0;
}
