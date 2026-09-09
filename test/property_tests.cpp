#include "runner_test_support.hpp"
#include <array>
#include <cfenv>
#include <forward_list>

namespace {
using verification::expect;
void integer_properties() {
    for (int left = -128; left <= 127; ++left) {
        for (int right = 0; right <= 255; ++right) {
            const auto a = static_cast<signed char>(left);
            const auto b = static_cast<unsigned char>(right);
            expect(chtest::compare_values(a, b, std::equal_to<>{}) == (left == right), "mixed integer equality");
            expect(chtest::compare_values(a, b, std::not_equal_to<>{}) == (left != right), "mixed integer inequality");
            expect(chtest::compare_values(a, b, std::less<>{}) == (left < right), "mixed integer less");
            expect(chtest::compare_values(b, a, std::greater<>{}) == (right > left), "reversed mixed integer greater");
            expect(chtest::compare_values(a, b, std::less_equal<>{}) == (left <= right), "mixed integer less equal");
            expect(chtest::compare_values(b, a, std::greater_equal<>{}) == (right >= left), "reversed mixed integer greater equal");
        }
    }
    const auto min = std::numeric_limits<std::int64_t>::min();
    const auto max = std::numeric_limits<std::uint64_t>::max();
    expect(chtest::compare_values(min, max, std::less<>{}), "64-bit minimum precedes unsigned maximum");
    expect(!chtest::compare_values(std::int64_t{-1}, max, std::equal_to<>{}), "64-bit values must not wrap");
    expect(chtest::compare_values(std::numeric_limits<std::int64_t>::max(), max, std::less<>{}), "64-bit unsigned upper half");
    expect(chtest::compare_values(false, 0u, std::equal_to<>{}), "boolean zero comparison");
}
void floating_properties(std::mt19937& rng) {
    for (int i = 0; i < 20000; ++i) {
        const auto a = static_cast<std::int64_t>(rng() % 2000001) - 1000000;
        const auto b = static_cast<std::int64_t>(rng() % 2000001) - 1000000;
        const auto abs = static_cast<std::int64_t>(rng() % 2000001);
        const auto rel = static_cast<std::int64_t>(rng() % 25);
        const auto diff = a >= b ? a - b : b - a;
        const auto magnitude = std::max(std::abs(a), std::abs(b));
        const auto left = static_cast<long double>(a) / 1024;
        const auto right = static_cast<long double>(b) / 1024;
        const auto absolute = static_cast<long double>(abs) / 1024;
        const auto relative = static_cast<long double>(rel) / 8;
        const bool expected = diff <= abs || diff * 8 <= magnitude * rel;
        expect(chtest::approx_near_abs(left, right, absolute) == (diff <= abs), "absolute comparison against exact integer oracle");
        expect(chtest::approx_compare_rel_abs(left, right, relative, absolute) == expected, "relative comparison against exact integer oracle");
        expect(chtest::approx_compare_rel_abs(right, left, relative, absolute) == expected, "approximate comparison symmetry");
    }
    volatile long double maximum = std::numeric_limits<long double>::max();
    std::feclearexcept(FE_ALL_EXCEPT);
    const bool near = chtest::approx_near_abs(maximum, -maximum, maximum);
    const int exceptions = std::fetestexcept(FE_OVERFLOW | FE_INVALID | FE_DIVBYZERO);
    expect(!near && exceptions == 0, "absolute comparison must not overflow intermediate values");
    expect(chtest::approx_compare_rel_abs(maximum, -maximum, 2, 0), "opposite extreme values relative boundary");
    const auto tiny = std::numeric_limits<long double>::denorm_min();
    expect(chtest::approx_near_abs(tiny, 0, tiny), "subnormal absolute boundary");
    expect(chtest::approx_compare_rel_abs(tiny, -tiny, 2, 0), "subnormal relative boundary");
    const auto infinity = std::numeric_limits<long double>::infinity();
    const auto nan = std::numeric_limits<long double>::quiet_NaN();
    for (const auto invalid : {-1.0L, infinity, nan}) {
        expect(!chtest::approx_near_abs(0, 0, invalid), "invalid absolute tolerance");
        expect(!chtest::approx_compare_rel_abs(0, 0, invalid, 0), "invalid relative tolerance");
        expect(!chtest::approx_compare_rel_abs(0, 0, 0, invalid), "invalid approximate absolute tolerance");
    }
    expect(chtest::approx_near_abs(-0.0, 0.0, 0), "signed zeros");
    expect(!chtest::approx_compare_rel_abs(nan, nan, 1, 1), "NaNs never approximately equal");
}
void sequence_properties(std::mt19937& rng) {
    for (int i = 1; i <= 1000; ++i) {
        const std::array<std::int64_t, 1> negative{{-i}};
        const std::array<std::uint64_t, 1> wrapped{{std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(i - 1)}};
        std::string note;
        expect(!chtest::contains_in(negative, wrapped[0]), "contains must not wrap negative elements to unsigned");
        expect(!chtest::contains_in(wrapped, negative[0]), "contains must not wrap a negative needle to unsigned");
        expect(!chtest::seq_equal_note(negative, wrapped, note), "sequence equality uses mathematical mixed integer comparison");
        expect(!chtest::seq_equal_note(wrapped, negative, note), "mixed sequence equality is symmetric");
    }
    for (int iteration = 0; iteration < 2000; ++iteration) {
        std::vector<int> a(rng() % 65), b;
        for (auto& value : a) value = static_cast<int>(rng() % 100);
        b = a;
        if (rng() % 2 && !b.empty()) ++b[rng() % b.size()];
        if (rng() % 3 == 0) b.push_back(101);
        const std::forward_list<int> forward(b.begin(), b.end());
        std::string note = "stale";
        const auto mismatch = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
        const bool equal = a == b;
        expect(chtest::seq_equal_note(a, forward, note) == equal, "sequence comparison against vector oracle");
        expect(chtest::container_size(forward) == static_cast<std::ptrdiff_t>(b.size()), "unsized forward container length");
        const int needle = static_cast<int>(rng() % 102);
        expect(chtest::contains_in(forward, needle) == (std::find(b.begin(), b.end(), needle) != b.end()), "contains model");
        if (equal) expect(note.empty(), "successful comparison clears stale note");
        else {
            const auto index = static_cast<std::size_t>(mismatch.first - a.begin());
            const bool size = mismatch.first == a.end() || mismatch.second == b.end();
            const auto expected = std::string(size ? "size mismatch at index " : "mismatch at index ") + std::to_string(index);
            expect(note.compare(0, expected.size(), expected) == 0, "first mismatch location");
        }
    }
}
void vector_properties(std::mt19937& rng) {
    chtest::ThreadSafeVector<int> actual;
    std::vector<int> model;
    for (int i = 0; i < 10000; ++i) {
        const auto index = static_cast<std::size_t>(rng() % (model.size() + 2));
        const auto value = static_cast<int>(rng() % 1000);
        switch (rng() % 9) {
        case 0: actual.push_back(value); model.push_back(value); break;
        case 1: actual.emplace_back(value); model.emplace_back(value); break;
        case 2:
            expect(actual.erase(index) == (index < model.size()), "erase result");
            if (index < model.size()) model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
            break;
        case 3:
            expect(actual.replace(index, value) == (index < model.size()), "replace result");
            if (index < model.size()) model[index] = value;
            break;
        case 4: actual.pop_back(); if (!model.empty()) model.pop_back(); break;
        case 5: actual.reserve(128); model.reserve(128); break;
        case 6: {
            auto copy = actual;
            auto moved = std::move(copy);
            actual = moved;
            expect(actual.to_vector() == model, "copy and move preserve values");
            break;
        }
        case 7:
            actual.for_each([](int& element) { ++element; });
            for (auto& element : model) ++element;
            break;
        case 8: actual.clear(i % 2 == 0); model.clear(); break;
        }
        expect(actual.to_vector() == model && actual.size() == model.size(), "vector state model");
        if (!model.empty()) expect(actual.front() == model.front() && actual.back() == model.back(), "copied endpoint access");
    }
    actual.clear(true);
    expect(actual.capacity() == 0, "vector release removes capacity");
    bool caught = false;
    try { (void)actual.at(0); } catch (const std::out_of_range&) { caught = true; }
    expect(caught, "at rejects out-of-range access");
}
void mock_properties(std::mt19937& rng) {
    chtest::MockFunction<int(int)> mock;
    std::vector<std::tuple<int>> history;
    std::size_t count = 0, limit = std::numeric_limits<std::size_t>::max();
    bool record = true;
    mock.setImpl([](int value) { return value * 2; });
    for (int i = 0; i < 10000; ++i) {
        switch (rng() % 8) {
        case 0: case 1: case 2: case 3: {
            const int value = static_cast<int>(rng() % 100);
            expect(mock(value) == value * 2, "mock implementation return");
            ++count;
            if (record && history.size() < limit) history.emplace_back(value);
            break;
        }
        case 4:
            limit = rng() % 32;
            mock.setHistoryLimit(limit);
            if (history.size() > limit) history.resize(limit);
            break;
        case 5:
            record = rng() % 2 != 0;
            mock.setRecordCalls(record);
            if (!record) history.clear();
            break;
        case 6: mock.reset(rng() % 2 != 0); count = 0; history.clear(); break;
        case 7: mock.reserveCalls(rng() % 64); break;
        }
        expect(mock.timesCalled() == count && mock.getCalls() == history, "mock history/count state model");
        const int needle = static_cast<int>(rng() % 100);
        expect(mock.calledWith(needle) == (std::find(history.begin(), history.end(), std::make_tuple(needle)) != history.end()), "mock historical lookup model");
    }
}
}
int main(int argc, char** argv) {
    return verification::main("properties", [&] {
        const unsigned seed = argc > 1 ? static_cast<unsigned>(std::stoul(argv[1])) : 42u;
        std::cout << "seed=" << seed << '\n';
        std::mt19937 rng(seed);
        integer_properties();
        floating_properties(rng);
        sequence_properties(rng);
        vector_properties(rng);
        mock_properties(rng);
    });
}
