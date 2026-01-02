#include "chtest.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

TEST_CASE("mock demo") {
    chtest::MockFunction<int(int, int)> add;

    add.setImpl([](int a, int b) { return a + b; });

    int r = add(2, 3);
    CHECK_EQ(r, 5);

    CHECK_CALLED(add);
    CHECK_CALLED_TIMES(add, 1);
    CHECK_CALLED_WITH(add, 2, 3);
}

TEST_CASE("mock multi counter") {
    chtest::MockFunction<int(int)> f;

    f.setImpl([](int x) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return x;
    });

    for (int i = 0; i < 5; ++i) {
        (void)f(i);
    }

    CHECK_CALLED_TIMES(f, 5);
    auto calls = f.getCalls();
    CHECK_EQ((int)calls.size(), 5);
    for (int i = 0; i < 5; ++i) {
        CHECK(calls[i] == std::make_tuple(i));
    }
}

TEST_CASE("mock sequential return values") {
    chtest::MockFunction<std::string(int)> f;

    f.setImpl([i = 0](int x) mutable {
        if (i == 0) {
            i++;
            return std::string("first");
        }
        if (i == 1) {
            i++;
            return std::string("second");
        }
        return std::to_string(x);
    });

    auto r1 = f(10);
    auto r2 = f(20);
    auto r3 = f(30);

    CHECK_EQ(r1, "first");
    CHECK_EQ(r2, "second");
    CHECK_EQ(r3, "30");

    CHECK_CALLED_TIMES(f, 3);
    CHECK_CALLED_WITH(f, 30);

    auto calls = f.getCalls();
    CHECK(calls[0] == std::make_tuple(10));
    CHECK(calls[1] == std::make_tuple(20));
    CHECK(calls[2] == std::make_tuple(30));
}
