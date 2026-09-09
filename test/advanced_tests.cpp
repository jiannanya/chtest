#include "chtest.hpp"

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("approx_near_pass") {
    double a = 1.2345;
    double b = 1.2344;
    CHECK_NEAR(a, b, 0.001);
}


TEST_CASE("thread_successful_checks") {
    auto abort_flag = ::chtest::make_case_abort();
    auto t = ::chtest::spawn_with_context(abort_flag, []() {
        THREAD_REQUIRE_NEAR(3.1415, 3.1416, 0.001);
    });
    t.join();
    CHECK(!*abort_flag);
}

TEST_CASE("check_approx_pass") {
    double expected = 100.0;
    double actual = 100.0009;
    CHECK_APPROX(actual, expected, 1e-5, 1e-4);
}


TEST_CASE_SKIP_IF("skip_runtime_true", true) {
    CHECK(false && "this case should have been skipped at runtime");
}

TEST_CASE_SKIP_IF("skip_runtime_false", false) {
    CHECK(true);
}

TEST_CASE_RETRY("flaky_per_case_retry", 1) {
    static int attempts = 0;
    ++attempts;
    if (attempts == 1) {
        CHECK(false && "first attempt fails (simulated flaky)");
    } else {
        CHECK(true);
    }
}


TEST_CASE_PRIO("priority_high", 10) { CHECK(true); }
TEST_CASE_PRIORITY("priority_low", 0) { CHECK(true); }

TEST_CASE_WITH_OPTS("with_opts_priority_retry", 5, 2, ([]() { return false; })) {
    static int attempts = 0;
    ++attempts;
    if (attempts == 1) {
        CHECK(false && "simulated flaky first attempt");
    } else {
        CHECK(true);
    }
}
