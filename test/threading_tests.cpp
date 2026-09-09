#include "chtest.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

TEST_CASE("mock multi-threaded counter") {
    chtest::MockFunction<int(int)> f;

    f.setImpl([](int x) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return x;
    });

    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        auto wrapped = ::chtest::with_current_case_context([&f, i]() {
            int r = f(i);
            CHECK_EQ(r, i);
        });
        threads.emplace_back(wrapped);
    }
    for (auto& t : threads) t.join();

    CHECK_CALLED_TIMES(f, 5);
    auto calls = f.getCalls();
    CHECK_EQ((int)calls.size(), 5);
}

TEST_CASE("threads: CHECK with with_current_case_context (basic)") {
    auto f = [](int i){ return i; }; // trivial function that should pass
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        auto wrapped = ::chtest::with_current_case_context([&f,i]() {
            int r = f(i);
            CHECK_EQ(r, i);
        });
        threads.emplace_back(wrapped);
    }
    for (auto &t : threads) t.join();
}

TEST_CASE("threads: ensure collector lifetime while threads run") {
    auto f = [](int i) { return i + 0; };
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(::chtest::with_current_case_context([&f, i]() {
            CHECK_EQ(f(i), i);
            ::chtest::ts_cout() << "thread " << i << " done\n";
        }));
    }
    for (auto& t : threads) t.join();
}

TEST_CASE("threads: CHECK without context (shows missing reporting)") {
    auto f = [](int i) { return i; };
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&f, i]() {
            CHECK_EQ(f(i), i);
        });
    }
    for (auto& t : threads) t.join();
}

TEST_CASE("threads: manual propagate route and tls_case_out (robust)") {
    auto parent_route = ::chtest::route();
    auto parent_out = ::chtest::tls_case_out;

    std::shared_ptr<std::string> keep_name;
    if (parent_route.active_name) {
        keep_name = std::make_shared<std::string>(parent_route.active_name);
        parent_route.active_name = keep_name->c_str();
    }

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([parent_route, parent_out, keep_name, i]() mutable {
            auto prev_route = ::chtest::route();
            auto prev_out = ::chtest::tls_case_out;
            ::chtest::route() = parent_route;
            ::chtest::tls_case_out = parent_out;

            ::chtest::ts_cout() << "child " << i << " route.active_name="
                                << (::chtest::route().active_name ? ::chtest::route().active_name : "<null>")
                                << "\n";

            CHECK_EQ(i, i);

            ::chtest::route() = prev_route;
            ::chtest::tls_case_out = prev_out;
        });
    }
    for (auto& t : threads) t.join();
}

TEST_CASE("threads: safe - child record_check + local buffer merge") {
    auto f = [](int i) { return i; };

    std::vector<std::thread> threads;
    std::vector<std::string> thread_outputs(5);

    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&f, i, &thread_outputs]() {
            std::ostringstream local_out;
            local_out << "thread " << i << " starting\n";
            int r = f(i);
            ::chtest::record_check(r == i,
                                   __FILE__,
                                   __LINE__,
                                   std::string("CHECK_EQ(f(i),") + std::to_string(i) + ")",
                                   "",
                                   false,
                                   ::chtest::current_quiet());
            local_out << "thread " << i << " done\n";
            thread_outputs[i] = local_out.str();
        });
    }
    for (auto& t : threads) t.join();

    {
        std::lock_guard<std::mutex> lk(::chtest::global_out_mutex());
        for (auto& s : thread_outputs) {
            if (!s.empty()) ::chtest::emit_output(s);
        }
    }
}



TEST_CASE("spawn_with_context simple") {
    auto f = [](int i) { return i; };
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(::chtest::spawn_with_context([&f, i]() {
            CHECK_EQ(f(i), i);
        }));
    }
    for (auto& t : threads) t.join();
}


TEST_CASE("feature: spawn_with_context basic - mock is called from threads") {
    chtest::MockFunction<int(int)> mf;
    mf.setImpl([](int x) { return x * 2; });

    const int N = 8;
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back(::chtest::spawn_with_context([&mf, i]() {
            int r = mf(i);
            CHECK_EQ(r, i * 2);
            ::chtest::ts_cout() << "thread " << i << " done\n";
        }));
    }
    for (auto& t : threads) t.join();

    CHECK_CALLED_TIMES(mf, N);
}


TEST_CASE("feature: concurrent writes to per-case buffer (per-buffer mutex)") {
    const int N = 16;
    chtest::MockFunction<int(int)> mf;
    mf.setImpl([](int x) { return x; });

    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back(::chtest::spawn_with_context([&mf, i]() {
            int r = mf(i);
            CHECK_EQ(r, i);
            ::chtest::ts_cout() << "concurrent thread wrote " << i << "\n";
        }));
    }

    for (auto& t : threads) t.join();

    CHECK_CALLED_TIMES(mf, N);
}

TEST_CASE("timeout: subcase simple") {
    SUBCASE("long") {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(true);
    }

    SUBCASE("short") {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(true);
    }
}
