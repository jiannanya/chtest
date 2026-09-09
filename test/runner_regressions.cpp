#include "chtest.hpp"
#include <array>
#include <cstdlib>

namespace {
int verified = 0;
std::string output;
void expect(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
    ++verified;
}
void add(const std::string& name, std::function<void()> fn, int retries = 0,
         int priority = 0, std::vector<std::string> tags = {}, std::function<bool()> skip = {}) {
    chtest::registry().push_back({name, std::move(fn), {}, false, std::move(tags), priority, retries, std::move(skip)});
}
int run(std::initializer_list<const char*> options = {}) {
    std::vector<std::string> args{"regressions", "--quiet", "--no-color"};
    for (const auto option : options) args.emplace_back(option);
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    output.clear();
    return chtest::run(static_cast<int>(argv.size()), argv.data());
}
void reset() {
    chtest::registry().clear();
    chtest::global_env() = nullptr;
    chtest::set_output_sink([](std::string_view text) { output.append(text); });
}
struct CounterEnv : chtest::Environment {
    int setups = 0, teardowns = 0, value = 0;
    bool throw_setup = false, throw_teardown = false;
    void setUp() override { ++setups; value = 7; if (throw_setup) throw std::runtime_error("setup"); }
    void tearDown() override { ++teardowns; value = -1; if (throw_teardown) throw std::runtime_error("teardown"); }
};
struct CopyTracked {
    inline static int copies = 0;
    CopyTracked() = default;
    CopyTracked(const CopyTracked&) { ++copies; }
    CopyTracked& operator=(const CopyTracked&) { ++copies; return *this; }
};
struct FormattedValue {};
std::ostream& operator<<(std::ostream& stream, FormattedValue) { return stream << '<' << std::setw(4) << 7 << '>'; }
const std::vector<CopyTracked> parameters(128);
TEST_CASE_PARAM("copy tracked parameter", parameters) { CHECK(true); (void)param; }

int fixture_teardowns = 0;
struct ThrowingFixture {
    void setUp() {}
    void tearDown() { ++fixture_teardowns; throw std::runtime_error("fixture teardown"); }
};
TEST_F(ThrowingFixture, "throwing fixture") {
    if (fixture_teardowns == 0) throw std::runtime_error("fixture body");
}

void verify_runner() {
    auto registrations = std::move(chtest::registry());
    expect(CopyTracked::copies == 128, "parameters must be copied once, not once per test");
    auto fixture = std::move(registrations.back());
    registrations.pop_back();
    reset();
    chtest::registry() = std::move(registrations);
    expect(run() == 0 && chtest::agg().cases == 128 && chtest::agg().checks == 128, "all parameter callbacks stay valid");
    expect(CopyTracked::copies == 128, "executing parameters must not copy them");

    reset();
    add("fatal", [] { REQUIRE(false); CHECK(false); });
    expect(run() == 1, "fatal assertion returns failure");
    expect(chtest::agg().checks == 1 && chtest::agg().failures == 1, "fatal failure is not counted twice");
    expect(chtest::route().mode == chtest::SubcaseMode::Normal && !chtest::tls_case_out && !chtest::tls_case_fail_count,
           "runner restores thread context");
    expect(run() == 1 && chtest::agg().checks == 1, "repeated run resets aggregate counters");

    reset();
    int attempts = 0;
    CounterEnv environment;
    chtest::global_env() = &environment;
    add("thread retry", [&] {
        CHECK_EQ(environment.value, 7);
        const auto attempt = ++attempts;
        auto child = chtest::spawn_with_context([attempt] { CHECK_GT(attempt, 1); });
        child.join();
    }, 1);
    expect(run() == 0 && attempts == 2, "child failures trigger retries and recovery succeeds");
    expect(environment.setups == 2 && environment.teardowns == 2, "hooks must bracket each retry");
    expect(chtest::agg().failures == 0 && chtest::agg().retried_failures == 1 && chtest::agg().retries == 1,
           "recovered failures are reported separately");

    reset();
    attempts = 0;
    add("global retry", [&] { CHECK_GT(++attempts, 2); });
    expect(run({"--retries", "2"}) == 0 && attempts == 3, "global retry count is honored");
    expect(chtest::agg().checks == 3 && chtest::agg().retried_failures == 2, "all retry checks are counted");

    reset();
    add("all retries fail", [] { REQUIRE(false); }, 2);
    expect(run() == 1 && chtest::agg().failures == 1 && chtest::agg().retries == 2, "exhausted retries retain final failure");

    reset();
    add("child fatal", [] { auto t = chtest::spawn_with_context([] { REQUIRE(false); }); t.join(); });
    expect(run() == 1 && chtest::agg().failures == 1 && chtest::agg().failed_cases == 1,
           "spawned fatal assertions are contained and attributed");
    reset();
    add("child exception", [] { auto t = chtest::spawn_with_context([] { throw 42; }); t.join(); });
    expect(run() == 1 && chtest::agg().failures == 1, "non-standard child exceptions are contained");

    reset();
    environment = CounterEnv{};
    environment.throw_setup = true;
    chtest::global_env() = &environment;
    int bodies = 0;
    add("setup exception", [&] { ++bodies; });
    expect(run() == 1 && bodies == 0 && environment.teardowns == 0, "failed setup prevents body and teardown");
    environment.throw_setup = false;
    environment.throw_teardown = true;
    expect(run() == 1 && bodies == 1 && environment.teardowns == 1, "teardown exceptions fail the case without escaping");
    reset();
    chtest::registry().push_back(std::move(fixture));
    expect(run() == 1 && fixture_teardowns == 1 && chtest::agg().failures == 2,
           "fixture body and cleanup errors must both survive");

    reset();
    int baseline = 0, parent = 0, first = 0, second = 0, sibling = 0;
    add("nested", [&] {
        CHECK_EQ(++baseline, 1);
        SUBCASE("same") {
            CHECK_EQ(++parent, 1);
            SUBCASE("leaf") { CHECK_EQ(++first, 1); }
            SUBCASE("leaf") { CHECK_EQ(++second, 1); }
            CHECK_EQ(parent, 1);
        }
        SUBCASE("same") { CHECK_EQ(++sibling, 1); }
    });
    expect(run() == 0 && chtest::agg().checks == 6 && chtest::agg().subcases == 4,
           "nested and same-name subcases execute exactly once with correct assertion counts");
    expect(baseline == 1 && parent == 1 && first == 1 && second == 1 && sibling == 1, "inactive assertion operands are not evaluated");
    expect(chtest::registry()[0].subcases.empty(), "discovered paths do not remain allocated in registry");

    reset();
    std::vector<std::string> names;
    for (int i = 0; i < 64; ++i) names.push_back("indexed parent with a long dynamic name " + std::to_string(i));
    add("indexed repeated nested paths", [&] {
        for (int visit = 0; visit < 2; ++visit) {
            for (const auto& name : names) {
                SUBCASE(name.c_str()) {
                    CHECK(true);
                    for (int nested_visit = 0; nested_visit < 2; ++nested_visit) {
                        SUBCASE("same leaf in different parents") { CHECK(true); }
                    }
                }
            }
        }
    });
    expect(run({"--repeat", "2"}) == 0 && chtest::agg().checks == 256 && chtest::agg().subcases == 256,
           "indexed discovery deduplicates paths and replay enters each target only once");

    reset();
    int reached = 0;
    add("fatal baseline", [&] { SUBCASE("registered first") { ++reached; } REQUIRE(false); });
    expect(run() == 1 && reached == 0 && chtest::agg().subcases == 0, "fatal discovery does not replay incomplete setup");

    reset();
    add("nested fatal", [] { SUBCASE("parent") { SUBCASE("fatal") { REQUIRE(false); } SUBCASE("passing") { CHECK(true); } } });
    expect(run() == 1 && chtest::agg().checks == 2 && chtest::agg().failures == 1, "fatal leaf still permits sibling execution");

    reset();
    int skip_calls = 0, executed = 0;
    add("skip", [&] { ++executed; }, 0, 0, {}, [&] { return ++skip_calls == 1; });
    expect(run({"--list"}) == 0 && skip_calls == 0, "listing does not execute skip predicates");
    expect(run({"--repeat", "2"}) == 0 && skip_calls == 2 && executed == 1 && chtest::agg().skipped == 1,
           "dynamic skip is reevaluated for each repetition");
    reset();
    add("skip throws", [] {}, 0, 0, {}, []() -> bool { throw std::runtime_error("skip"); });
    expect(run() == 1 && chtest::agg().failed_cases == 1, "skip predicate exceptions are failures");

    reset();
    add("Fast Math", [] { CHECK(true); }, 0, 0, {"fast", "math"});
    add("Slow Math", [] { CHECK(true); }, 0, 0, {"slow", "math"});
    add("Fast IO", [] { CHECK(true); }, 0, 0, {"fast", "io"});
    add("Untagged", [] { CHECK(true); });
    expect(run({"--tag", "fast", "--tag-all", "math", "--not-tag", "io"}) == 0 && chtest::agg().cases == 1,
           "tag filter groups compose");
    expect(run({"--test", "mAtH"}) == 0 && chtest::agg().cases == 2, "name matching is case insensitive");
    expect(run({"--no-tag"}) == 0 && chtest::agg().cases == 1, "untagged filter works");
    expect(run({"--test", "does not exist"}) == 0 && chtest::agg().cases == 0, "empty selection has consistent counters");
    expect(run({"--help"}) == 0 && output.find("--timeout") != std::string::npos, "help returns to embedding program");
    for (const auto option : {"--threads", "--repeat", "--retries", "--timeout", "--shuffle", "--buffer-limit"}) {
        expect(run({option}) == 2, "missing numeric value must be rejected");
        expect(run({option, "12garbage"}) == 2, "numeric trailing garbage must be rejected");
        expect(run({option, "-1"}) == 2, "negative numeric value must be rejected");
        expect(run({option, "999999999999999999999999999999"}) == 2, "overflow must be rejected");
    }
    expect(run({"--threads", "0"}) == 2 && run({"--repeat", "0"}) == 2 && run({"--buffer-limit", "0"}) == 2,
           "positive counts cannot be zero");
    expect(run({"--unknown"}) == 2 && run({"--tag"}) == 2 && run({"--test"}) == 2, "invalid CLI returns usage error");

    reset();
    std::vector<int> order;
    for (int i = 0; i < 12; ++i) add(std::to_string(i), [&, i] { order.push_back(i); }, 0, i % 3);
    expect(run({"--shuffle", "19"}) == 0, "shuffled run succeeds");
    const auto first_order = order;
    order.clear();
    expect(run({"--shuffle", "19"}) == 0 && order == first_order, "seed gives deterministic order");
    expect(std::is_sorted(order.begin(), order.end(), [](int a, int b) { return a % 3 > b % 3; }), "priority dominates shuffle");

    reset();
    std::atomic<int> active{0}, peak{0}, completed{0};
    for (int i = 0; i < 48; ++i) add("worker " + std::to_string(i), [&] {
        const int now = active.fetch_add(1) + 1;
        int previous = peak.load();
        while (previous < now && !peak.compare_exchange_weak(previous, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        active.fetch_sub(1);
        completed.fetch_add(1);
        CHECK(true);
    });
    expect(run({"--threads", "3", "--repeat", "2"}) == 0 && completed == 96, "bounded workers execute all scheduled cases");
    expect(peak > 1 && peak <= 3, "threads N is a hard limit for case workers");
    expect(chtest::agg().case_times.empty() && chtest::agg().subcase_times.empty(), "timings do not consume storage by default");
    expect(run({"--threads", "3", "--timings"}) == 0 && chtest::agg().case_times.size() == 48, "optional timings are collected");
    expect(run({"--threads", "3"}) == 0 && chtest::agg().case_times.capacity() == 0, "subsequent runs release timing memory");

    reset();
    bool observed_abort = false;
    add("cooperative timeout", [&] {
        while (!chtest::tls_case_abort->load()) std::this_thread::yield();
        observed_abort = true;
    });
    expect(run({"--timeout", "20"}) == 1 && observed_abort && chtest::agg().timeouts == 1 && chtest::agg().failures == 1,
           "watchdog signals cooperative abort and records one timeout");
    reset();
    bool finished = false;
    add("uncooperative timeout", [&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); finished = true; });
    expect(run({"--timeout", "5"}) == 1 && finished && chtest::agg().timeouts == 1, "timeout waits safely for uncooperative code");

    reset();
    add("buffered output", [] { for (int i = 0; i < 1000; ++i) chtest::ts_cout() << "abcdefghijklmnop\n"; });
    std::size_t received = 0, maximum_chunk = 0;
    chtest::set_output_sink([&](std::string_view text) { received += text.size(); maximum_chunk = std::max(maximum_chunk, text.size()); });
    expect(run({"--buffer-limit", "128"}) == 0 && received >= 17000 && maximum_chunk <= 512, "output buffering is bounded and lossless");
    chtest::set_output_sink([](std::string_view) { throw std::runtime_error("sink"); });
    expect(run() == 1, "throwing output sink must fail safely without terminate");
    reset();
    const std::string embedded("a\0b", 3);
    chtest::ts_cout() << "prefix:" << embedded << std::string_view{} << ':' << std::string_view(embedded);
    expect(output == std::string("prefix:a\0b:a\0b", 14), "text fast path preserves embedded nulls and empty views");
    for (const auto size : {255, 256, 257, 2048}) {
        output.clear();
        const std::string message(static_cast<std::size_t>(size), 'x');
        chtest::ts_cout() << message << 'y' << std::string_view(message);
        expect(output == message + 'y' + message, "inline and heap text boundaries preserve bytes");
        output.clear();
        chtest::ts_cout() << message << ':' << 42;
        expect(output == message + ":42", "inline and heap text preserve prefix when stream is initialized");
    }
    output.clear();
    std::ostringstream expected;
    expected << "prefix:" << std::hex << std::showbase << 255 << ':' << std::setfill('_') << std::setw(6)
             << "x" << ':' << FormattedValue{} << ':' << std::dec << std::fixed << std::setprecision(2) << 1.25 << std::endl;
    chtest::ts_cout() << "prefix:" << std::hex << std::showbase << 255 << ':' << std::setfill('_') << std::setw(6)
                     << "x" << ':' << FormattedValue{} << ':' << std::dec << std::fixed << std::setprecision(2) << 1.25 << std::endl;
    expect(output == expected.str(), "stream fallback preserves manipulators, width, numbers and custom formatters");
    output.clear();
    {
        chtest::case_output_collector collector(97);
        std::vector<std::thread> writers;
        for (int worker = 0; worker < 4; ++worker) writers.emplace_back(chtest::with_current_case_context([worker] {
            const std::string message(47, static_cast<char>('a' + worker));
            for (int i = 0; i < 100; ++i) chtest::ts_cout() << message << '\n';
        }));
        for (auto& writer : writers) writer.join();
    }
    expect(output.size() == 4 * 100 * 48, "concurrent text chunks are not lost");
    for (char marker = 'a'; marker <= 'd'; ++marker)
        expect(std::count(output.begin(), output.end(), marker) == 4700, "concurrent text chunks preserve each producer");
    reset();
    add("nested run", [] { char name[] = "nested"; char* argv[] = {name}; CHECK_EQ(chtest::run(1, argv), 2); });
    expect(run() == 0, "nested runner invocation is rejected without deadlock or counter reset");

    reset();
    std::shared_ptr<chtest::PerCaseBuffer> old_buffer;
    {
        chtest::case_output_collector outer;
        auto* previous = chtest::tls_case_out;
        { chtest::case_output_collector inner; chtest::ts_cout() << "inner"; }
        expect(chtest::tls_case_out == previous, "nested collectors restore prior output destination");
        old_buffer = outer.buf;
    }
    chtest::append_output(old_buffer.get(), "late");
    expect(output.find("innerlate") != std::string::npos, "closed shared buffers forward late output");
    int callbacks = 0;
    chtest::set_output_sink([&](std::string_view) { ++callbacks; chtest::ts_cout() << "sink reentry\n"; });
    chtest::emit_output("trigger");
    expect(callbacks == 1, "sink reentry does not recurse or deadlock");
    callbacks = 0;
    { chtest::case_output_collector collector(32); chtest::ts_cout() << std::string(40, 'x'); }
    expect(callbacks == 1, "sink reentry bypasses active case buffers");

    reset();
    chtest::route().mode = chtest::SubcaseMode::Discovery;
    auto context_failures = std::make_shared<std::atomic<chtest::count_type>>(0);
    chtest::tls_case_fail_count_keep = context_failures;
    chtest::tls_case_fail_count = context_failures.get();
    auto wrapped = chtest::with_current_case_context([] { throw std::runtime_error("wrapped"); });
    chtest::route() = chtest::RouteState{};
    chtest::tls_case_fail_count = nullptr;
    chtest::tls_case_fail_count_keep.reset();
    try { wrapped(); } catch (const std::runtime_error&) {}
    expect(!chtest::tls_case_fail_count && chtest::route().mode == chtest::SubcaseMode::Normal,
           "context wrapper restores TLS when its callable throws");
}
}

int main() {
    try {
        verify_runner();
        chtest::clear_output_sink();
        std::cout << "runner regressions: " << verified << " independent checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        chtest::clear_output_sink();
        std::cerr << "REGRESSION FAILURE: " << error.what() << "\nLast runner output:\n" << output;
        return 1;
    }
}
