#include "chtest.hpp"

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("approx_near_pass") {
    double a = 1.2345;
    double b = 1.2344;
    CHECK_NEAR(a, b, 0.001);
}

TEST_CASE("thread_set_abort_on_failure") {
    auto abort_flag = ::chtest::make_case_abort();
    std::vector<int> numbers = {1, 3, 5, 7};
    auto t = ::chtest::spawn_with_context(abort_flag, [&numbers]() {
        THREAD_REQUIRE_CONTAINS(numbers, 2);
    });
    t.join();
    REQUIRE(*abort_flag);
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

TEST_CASE("thread_check_approx_sets_abort") {
    auto abort_flag = ::chtest::make_case_abort();
    auto t = ::chtest::spawn_with_context(abort_flag, []() {
        double a = 0.00123;
        double b = 0.00100;
        THREAD_REQUIRE_APPROX(a, b, 1e-4, 1e-5);
    });
    t.join();
    REQUIRE(*abort_flag);
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

TEST_CASE("flaky_global_retry_example") {
    static int attempts = 0;
    ++attempts;
    if (attempts == 1) {
        CHECK(false && "global first attempt fails (simulated flaky)");
    } else {
        CHECK(true);
    }
}

static std::vector<std::string> __priority_run_order;

TEST_CASE_PRIORITY("priority_high", 10) {
    __priority_run_order.push_back("high");
}

TEST_CASE_PRIORITY("priority_low_and_check", 0) {
    __priority_run_order.push_back("low");
    CHECK(__priority_run_order.size() >= 2);
    CHECK(__priority_run_order[0] == "high");
    CHECK(__priority_run_order[1] == "low");
}

TEST_CASE_WITH_OPTS("with_opts_priority_retry", 5, 2, ([]() { return false; })) {
    static int attempts = 0;
    ++attempts;
    if (attempts == 1) {
        CHECK(false && "simulated flaky first attempt");
    } else {
        CHECK(true);
    }
}
