#include "runner_test_support.hpp"
#include <array>

namespace {
using namespace verification;
void throw_error() { throw std::runtime_error("expected"); }
struct Entry { const char* name; std::function<void()> pass, fail; };
#define MATRIX(P) \
    Entry{#P, [] { P(true); }, [] { P(false); }}, \
    Entry{#P "_EQ", [] { P##_EQ(1, 1); }, [] { P##_EQ(1, 2); }}, \
    Entry{#P "_NE", [] { P##_NE(1, 2); }, [] { P##_NE(1, 1); }}, \
    Entry{#P "_LT", [] { P##_LT(1, 2); }, [] { P##_LT(2, 1); }}, \
    Entry{#P "_LE", [] { P##_LE(2, 2); }, [] { P##_LE(2, 1); }}, \
    Entry{#P "_GT", [] { P##_GT(2, 1); }, [] { P##_GT(1, 2); }}, \
    Entry{#P "_GE", [] { P##_GE(2, 2); }, [] { P##_GE(1, 2); }}, \
    Entry{#P "_NEAR", [] { P##_NEAR(1, 2, 1); }, [] { P##_NEAR(1, 2, .5); }}, \
    Entry{#P "_APPROX", [] { P##_APPROX(1, 2, .5, 0); }, [] { P##_APPROX(1, 2, .25, 0); }}, \
    Entry{#P "_CONTAINS", [] { P##_CONTAINS(std::string("abc"), 'b'); }, [] { P##_CONTAINS(std::string("abc"), 'd'); }}, \
    Entry{#P "_SIZE", [] { P##_SIZE(std::string("abc"), 3); }, [] { P##_SIZE(std::string("abc"), -1); }}, \
    Entry{#P "_SEQ_EQ", [] { P##_SEQ_EQ(std::string("abc"), std::string("abc")); }, [] { P##_SEQ_EQ(std::string("abc"), std::string("abd")); }}, \
    Entry{#P "_THROWS", [] { P##_THROWS(throw_error()); }, [] { P##_THROWS(1 + 1); }}, \
    Entry{#P "_NOTHROW", [] { P##_NOTHROW(1 + 1); }, [] { P##_NOTHROW(throw_error()); }}

void assertion_matrix() {
    const Entry entries[]{MATRIX(CHECK), MATRIX(REQUIRE), MATRIX(THREAD_REQUIRE)};
    for (const auto& entry : entries) {
        for (const bool failure : {false, true}) {
            reset();
            int continued = 0;
            bool aborted = false;
            const std::string name = entry.name;
            const bool fatal = name.compare(0, 7, "REQUIRE") == 0;
            const bool threaded = name.compare(0, 7, "THREAD_") == 0;
            add(name, [&] {
                (failure ? entry.fail : entry.pass)();
                ++continued;
                aborted = chtest::tls_case_abort->load();
            });
            expect(run() == (failure ? 1 : 0), entry.name);
            expect(chtest::agg().checks == 1 && chtest::agg().failures == (failure ? 1u : 0u), "one assertion creates one result");
            expect(continued == (failure && fatal ? 0 : 1), "fatal/continuing assertion contract");
            expect(aborted == (failure && threaded), "thread-required assertion cancellation contract");
            expect(chtest::agg().failed_cases == (failure ? 1u : 0u), "failure attributed to owning case");
            if (failure) expect(output.find("assertion_contracts.cpp:") != std::string::npos, "diagnostic points to user assertion");
        }
    }
}
#undef MATRIX

struct Diagnostic {
    int value, mode;
    inline static int prints = 0;
    bool operator==(const Diagnostic& other) const { return value == other.value; }
};
std::ostream& operator<<(std::ostream& stream, const Diagnostic& value) {
    ++Diagnostic::prints;
    if (value.mode == 0) throw std::runtime_error("printer");
    if (value.mode == 1) throw 42;
    stream.setstate(std::ios::badbit);
    return stream;
}
void diagnostic_contracts() {
    for (int mode = 0; mode < 3; ++mode) {
        reset();
        int continued = 0;
        Diagnostic::prints = 0;
        add("successful checks do not format values", [&] { CHECK_EQ((Diagnostic{1, mode}), (Diagnostic{1, mode})); });
        expect(run() == 0 && Diagnostic::prints == 0, "successful values never invoke a throwing printer");
        reset();
        add("failed check with broken formatter", [&] {
            CHECK_EQ((Diagnostic{1, mode}), (Diagnostic{2, mode}));
            ++continued;
            CHECK(true);
        });
        expect(run() == 1 && continued == 1 && chtest::agg().checks == 2 && chtest::agg().failures == 1,
               "broken printer must not terminate a nonfatal assertion");
        expect(output.find("lhs=<formatting ") != std::string::npos, "broken printer has fallback diagnostic");
        reset();
        continued = 0;
        add("failed requirement with broken formatter", [&] {
            REQUIRE_EQ((Diagnostic{1, mode}), (Diagnostic{2, mode}));
            ++continued;
        });
        expect(run() == 1 && continued == 0 && chtest::agg().checks == 1 && chtest::agg().failures == 1,
               "broken printer preserves fatal assertion behavior");
        reset();
        add("sequence mismatch with broken formatter", [&] {
            const std::array<Diagnostic, 1> a{{{1, mode}}}, b{{{2, mode}}};
            CHECK_SEQ_EQ(a, b);
            CHECK(true);
        });
        expect(run() == 1 && chtest::agg().checks == 2 && output.find("mismatch at index 0") != std::string::npos,
               "sequence diagnostic preserves mismatch when element formatting fails");
    }
    reset();
    add("nested fatal must not be swallowed by throws", [] { CHECK_THROWS([] { REQUIRE(false); }()); });
    expect(run() == 1 && chtest::agg().checks == 1, "exception assertion does not swallow a fatal assertion");
    reset();
    add("nested fatal must not be swallowed by nothrow", [] { CHECK_NOTHROW([] { REQUIRE(false); }()); });
    expect(run() == 1 && chtest::agg().checks == 1, "nothrow assertion does not duplicate a fatal assertion");
    reset();
    {
        chtest::ContextRestore restore;
        chtest::route().mode = chtest::SubcaseMode::Discovery;
        bool caught = false;
        try { THREAD_REQUIRE(false); } catch (const chtest::AssertionFailure&) { caught = true; }
        expect(caught, "thread requirement without cancellation context is fatal");
    }
}
void large_counts() {
    reset();
    constexpr std::uint64_t boundary = std::uint64_t{1} << 32;
    add("large check count", [] {
        chtest::agg().checks = boundary - 1;
        CHECK(true);
    });
    expect(run() == 0 && chtest::agg().checks == boundary, "check counter crosses 32 bits");
    expect(output.find("checks=4294967296") != std::string::npos, "summary does not narrow 64-bit counts");
    reset();
    int attempt = 0;
    add("large retry failure count", [&] {
        if (++attempt == 1) {
            chtest::agg().failures = boundary - 1;
            *chtest::tls_case_fail_count = boundary - 1;
            CHECK(false);
        } else CHECK(true);
    }, 1);
    expect(run() == 0 && chtest::agg().failures == 0 && chtest::agg().retried_failures == boundary,
           "retry subtraction and history preserve 64-bit counts");
}
struct FailedSync : std::stringbuf { int sync() override { return -1; } };
struct RestoreCout {
    std::streambuf* buffer = std::cout.rdbuf();
    std::ios::iostate exceptions = std::cout.exceptions();
    ~RestoreCout() {
        std::cout.exceptions(std::ios::goodbit);
        std::cout.rdbuf(buffer);
        std::cout.clear();
        std::cout.exceptions(exceptions);
    }
};
void flush_failures() {
    for (bool throwing : {false, true}) {
        reset();
        add("output flush failure", [] { CHECK(true); });
        int result = -1;
        {
            FailedSync broken;
            RestoreCout restore;
            std::cout.rdbuf(&broken);
            if (throwing) std::cout.exceptions(std::ios::badbit);
            result = run({"--no-buffer"});
        }
        expect(result == 1, "stdout flush failure is contained and makes run fail");
    }
}
}
int main() {
    return verification::main("assertion contracts", [] {
        assertion_matrix();
        diagnostic_contracts();
        large_counts();
        flush_failures();
    });
}
