#include "chtest.hpp"
#include <array>
#include <forward_list>
#include <limits>

TEST_CASE("assertions: single evaluation and safe if else") {
    int left = 0, right = 0, tolerance = 0;
    if (true) CHECK_EQ(++left, ++right); else CHECK(false);
    if (false) REQUIRE(false); else ++left;
    CHECK_EQ(left, 2);
    CHECK_EQ(right, 1);
    THREAD_REQUIRE_NEAR(++left, ++right, ++tolerance);
    CHECK_EQ(left, 3);
    CHECK_EQ(right, 2);
    CHECK_EQ(tolerance, 1);
    int rel = 0, abs = 0;
    THREAD_REQUIRE_APPROX(++left, ++right, ++rel, ++abs);
    CHECK_EQ(left, 4);
    CHECK_EQ(right, 3);
    CHECK_EQ(rel, 1);
    CHECK_EQ(abs, 1);
    std::vector<int> values{1, 2, 3};
    int accesses = 0, needle = 0, size = 2;
    auto container = [&]() -> const auto& { ++accesses; return values; };
    THREAD_REQUIRE_CONTAINS(container(), ++needle);
    THREAD_REQUIRE_SIZE(container(), ++size);
    THREAD_REQUIRE_SEQ_EQ(container(), container());
    CHECK_EQ(accesses, 4);
    CHECK_EQ(needle, 1);
    CHECK_EQ(size, 3);
}

TEST_CASE("assertions: signed unsigned comparisons") {
    CHECK_NE(-1, std::numeric_limits<unsigned>::max());
    CHECK_LT(-1, 0u);
    CHECK_GT(0u, -1);
    CHECK_LE(-1, 0u);
    CHECK_GE(0u, -1);
    CHECK_EQ(true, 1);
    CHECK_EQ(1u, 1);
    struct { unsigned value : 3; } bits{3};
    CHECK_EQ(bits.value, 3);
}

TEST_CASE("assertions: floating point boundaries") {
    const auto inf = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_NEAR(inf, inf, 0);
    CHECK_APPROX(-inf, -inf, 0, 0);
    CHECK(!chtest::approx_near_abs(inf, -inf, 1));
    CHECK(!chtest::approx_near_abs(nan, nan, 1));
    CHECK(!chtest::approx_near_abs(1, 1, -1));
    CHECK(!chtest::approx_compare_rel_abs(1, 1, nan, 0));
    const auto huge = std::numeric_limits<long double>::max();
    CHECK(!chtest::approx_compare_rel_abs(huge, -huge, 1, 0));
    CHECK(chtest::approx_compare_rel_abs(huge, -huge, 2, 0));
    CHECK_APPROX(100.0009, 100.0, 1e-5, 1e-4);
}

TEST_CASE("assertions: arrays and forward containers") {
    const int array[] = {1, 2, 3};
    const std::forward_list<int> forward{1, 2, 3};
    CHECK_SIZE(array, 3);
    CHECK_SIZE(forward, 3);
    CHECK_SEQ_EQ(array, forward);
    std::string note = "old failure";
    CHECK(chtest::seq_equal_note(array, forward, note));
    CHECK(note.empty());
    CHECK(!chtest::seq_equal_note(array, std::vector<int>{1, 2}, note));
    CHECK_CONTAINS(note, '2');
}

TEST_CASE("threads: move only callable and arguments") {
    int value = 0;
    auto thread = chtest::spawn_with_context([owned = std::make_unique<int>(7), &value]() {
        value = *owned;
        REQUIRE_EQ(value, 7);
    });
    thread.join();
    auto argument = chtest::spawn_with_context([&](std::unique_ptr<int> owned) { value += *owned; }, std::make_unique<int>(3));
    argument.join();
    CHECK_EQ(value, 10);
    auto wrapped = chtest::with_current_case_context([owned = std::make_unique<int>(9)] { return *owned; });
    CHECK_EQ(wrapped(), 9);
}

TEST_CASE("mock: history stores owned values and searches any call") {
    chtest::MockFunction<void(const std::string&)> mock;
    std::string text = "before";
    mock(text);
    text = "after";
    mock(text);
    CHECK_CALLED_WITH(mock, std::string("before"));
    CHECK_CALLED_WITH(mock, std::string("after"));
    CHECK_EQ(std::get<0>(mock.getCalls().front()), "before");
}

TEST_CASE("mock: bounded history count only and release") {
    chtest::MockFunction<int(int)> mock;
    mock.setHistoryLimit(3);
    mock.reserveCalls(10);
    for (int i = 0; i < 100; ++i) mock(i);
    CHECK_SIZE(mock.getCalls(), 3);
    CHECK_CALLED_TIMES(mock, 100);
    mock.setRecordCalls(false);
    mock(101);
    CHECK(mock.getCalls().empty());
    CHECK_CALLED_TIMES(mock, 101);
    mock.reset(true);
    CHECK_CALLED_TIMES(mock, 0);
}

TEST_CASE("mock: move only arguments and reference returns") {
    chtest::MockFunction<int(std::unique_ptr<int>)> mock;
    mock.setImpl([](std::unique_ptr<int> value) { return *value; });
    CHECK_EQ(mock(std::make_unique<int>(11)), 11);
    CHECK_CALLED_TIMES(mock, 1);
    CHECK_THROWS(mock.setRecordCalls(true));
    chtest::MockFunction<int&()> reference;
    CHECK_THROWS(reference());
    int value = 8;
    reference.setImpl([&]() -> int& { return value; });
    reference() = 9;
    CHECK_EQ(value, 9);
}

TEST_CASE("mock: concurrent mutable implementation and replacement") {
    chtest::MockFunction<int()> mock;
    mock.setImpl([value = 0]() mutable { return ++value; });
    std::atomic<int> sum{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back(chtest::spawn_with_context([&] {
        for (int j = 0; j < 100; ++j) sum.fetch_add(mock(), std::memory_order_relaxed);
    }));
    for (auto& thread : threads) thread.join();
    CHECK_EQ(sum.load(), 400 * 401 / 2);
    auto replacement = chtest::spawn_with_context([&] {
        for (int i = 0; i < 100; ++i) mock.setImpl([] { return 1; });
    });
    for (int i = 0; i < 100; ++i) CHECK_GT(mock(), 0);
    replacement.join();
    CHECK_CALLED_TIMES(mock, 500);
}

TEST_CASE("mock: implementation can inspect its own history") {
    chtest::MockFunction<int(int)> mock;
    mock.setImpl([&](int value) {
        CHECK_CALLED(mock);
        if (value > 0) return mock(value - 1);
        return 42;
    });
    CHECK_EQ(mock(2), 42);
    CHECK_CALLED_TIMES(mock, 3);
}

TEST_CASE_TAG("tags: initializer list with commas", {"regression", "fast", "unit"}) { CHECK(true); }

TEST_CASE("subcases: nested and identical sibling names") {
    int value = 1;
    CHECK_EQ(value, 1);
    SUBCASE("parent") {
        ++value;
        CHECK_EQ(value, 2);
        SUBCASE("child") { CHECK_EQ(value, 2); }
        SUBCASE("child") { CHECK_EQ(value, 2); }
        CHECK_EQ(value, 2);
    }
    SUBCASE("parent") { CHECK_EQ(value, 1); }
}

static const std::array<int, 3> const_params{{2, 4, 8}};
TEST_CASE_PARAM("parameters: const container", const_params) { CHECK_GT(param, 0); }

TEST_CASE("assertions: bitfields explicit bool and temporary subobjects") {
    struct Bits { unsigned value : 3; unsigned tolerance : 2; } bits{3, 1};
    const std::array<int, 3> values{{1, 2, 3}};
    CHECK_NEAR(bits.value, 3, bits.tolerance);
    REQUIRE_NEAR(3, bits.value, bits.tolerance);
    THREAD_REQUIRE_NEAR(bits.value, bits.value, 0);
    CHECK_APPROX(bits.value, 3, 0, bits.tolerance);
    REQUIRE_APPROX(3, bits.value, bits.tolerance, 0);
    THREAD_REQUIRE_APPROX(bits.value, bits.value, 0, 0);
    CHECK_SIZE(values, bits.value);
    REQUIRE_SIZE(values, bits.value);
    THREAD_REQUIRE_SIZE(values, bits.value);
    CHECK_CONTAINS(values, bits.value);
    REQUIRE_CONTAINS(values, bits.value);
    THREAD_REQUIRE_CONTAINS(values, bits.value);
    struct ExplicitBool { explicit operator bool() const { return true; } };
    CHECK(ExplicitBool{});
    CHECK_EQ(std::vector<int>{7}.front(), 7);
    CHECK_NEAR(std::vector<double>{1.0}.front(), 1.0, 0);
    CHECK_APPROX(std::vector<double>{1.0}.front(), 1.0, 0, 0);
    CHECK_SIZE(std::vector<std::string>{"abc"}.front(), 3);
    CHECK_CONTAINS(std::vector<std::string>{"abc"}.front(), 'b');
}

TEST_CASE("threads: member functions reference arguments and nested contexts") {
    struct Target {
        void update(int& value, std::unique_ptr<int> increment) { value += *increment; CHECK_EQ(value, 7); }
    } target;
    int value = 3;
    auto thread = chtest::spawn_with_context(&Target::update, &target, std::ref(value), std::make_unique<int>(4));
    thread.join();
    CHECK_EQ(value, 7);
    auto wrapper = chtest::with_current_case_context([&]() -> int& { return value; });
    wrapper() = 9;
    CHECK_EQ(value, 9);
    auto child = chtest::spawn_with_context([] {
        auto grandchild = chtest::spawn_with_context([] { CHECK(true); });
        grandchild.join();
        CHECK(true);
    });
    child.join();
}

static const std::vector<bool> boolean_params{true, false, true};
TEST_CASE_PARAM("parameters: proxy container", boolean_params) { CHECK(param == true || param == false); }
static const std::array<int, 0> empty_params{};
TEST_CASE_PARAM("parameters: empty container registers nothing", empty_params) { (void)param; REQUIRE(false); }
