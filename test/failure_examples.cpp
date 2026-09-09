#define CH_TEST_MAIN
#include "chtest.hpp"
#include <list>

// Deliberately failing examples are tested as a separate executable.

TEST_CASE("seq_eq_mismatch_note") {
    std::vector<int> a = {1, 2, 4};
    std::list<int> b = {1, 2, 3};
    CHECK_SEQ_EQ(a, b);
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

TEST_CASE("flaky_global_retry_example") {
    static int attempts = 0;
    ++attempts;
    if (attempts == 1) {
        CHECK(false && "global first attempt fails (simulated flaky)");
    } else {
        CHECK(true);
    }
}

TEST_CASE("threads: multiple failures accumulate") {
    auto f = [](int i) { return i % 3; };
    std::vector<std::thread> threads;
    for (int i = 0; i < 9; ++i) {
        threads.emplace_back(::chtest::with_current_case_context([&f, i]() {
            CHECK_EQ(f(i), i);
        }));
    }
    for (auto& t : threads) t.join();
}

TEST_CASE("threads: safely handle exceptions from child thread") {
    auto dangerous = []() { throw std::runtime_error("boom"); };
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back(::chtest::with_current_case_context([i, dangerous]() {
            try {
                dangerous();
                CHECK(true);
            } catch (const std::exception& e) {
                ::chtest::record_check(false,
                                       __FILE__,
                                       __LINE__,
                                       "uncaught exception in child thread",
                                       std::string("thread ") + std::to_string(i) + ": " + e.what(),
                                       false,
                                       ::chtest::current_quiet());
            }
        }));
    }
    for (auto& t : threads) t.join();
}

TEST_CASE("spawn_with_context + THREAD_REQUIRE abort example") {
    auto abort_flag = ::chtest::make_case_abort();

    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back(::chtest::spawn_with_context(abort_flag, [i]() {
            THREAD_REQUIRE(i % 2 == 0);
            CHECK_EQ(i % 2, 0);
        }));
    }
    for (auto& t : threads) t.join();

    if (abort_flag->load()) {
        ::chtest::ts_cout() << "Case observed THREAD_REQUIRE failure(s) in child threads\n";
    }
}

TEST_CASE("feature: THREAD_REQUIRE sets per-case abort flag and other threads observe it") {
    auto abort_flag = ::chtest::make_case_abort();
    std::atomic<int> observed{0};
    const int N = 6;
    std::vector<std::thread> threads;

    for (int i = 0; i < N; ++i) {
        threads.emplace_back(::chtest::spawn_with_context(abort_flag, [i, &observed]() {
            if (i == 0) {
                THREAD_REQUIRE(false);
                ::chtest::ts_cout() << "thread 0 set abort\n";
            } else {
                for (int k = 0; k < 50; ++k) {
                    if (::chtest::tls_case_abort && ::chtest::tls_case_abort->load()) {
                        observed.fetch_add(1);
                        ::chtest::ts_cout() << "thread " << i << " observed abort\n";
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                CHECK(true);
            }
        }));
    }

    for (auto& t : threads) t.join();

    CHECK(abort_flag->load() == true);
    CHECK(observed.load() >= 1);
}
