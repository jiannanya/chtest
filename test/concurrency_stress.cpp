#include "runner_test_support.hpp"
#include <array>
#include <future>
#include <set>

namespace {
using namespace verification;
// A start gate creates overlap without depending on short sleep durations.
class Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned remaining;
public:
    explicit Gate(unsigned count) : remaining(count) {}
    void arrive() {
        std::unique_lock<std::mutex> lock(mutex);
        if (--remaining == 0) changed.notify_all();
        else changed.wait(lock, [&] { return remaining == 0; });
    }
};
void retry_stress() {
    reset();
    constexpr int cases = 32, repeats = 3;
    std::array<std::atomic<int>, cases> attempts{};
    for (int i = 0; i < cases; ++i) {
        attempts[i] = 0;
        add("retry owner " + std::to_string(i), [&, i] {
            const auto attempt = ++attempts[i];
            CHECK(true);
            auto child = chtest::spawn_with_context([attempt] { CHECK_EQ(attempt % 2, 0); });
            child.join();
        }, 1);
    }
    for (int i = 0; i < 8; ++i) add("skipped " + std::to_string(i), [] { REQUIRE(false); }, 0, [] { return true; });
    expect(run({"--threads", "8", "--repeat", "3", "--shuffle", "4294967295", "--buffer-limit", "31"}) == 0,
           "parallel child failures recover on their owning cases");
    expect(chtest::agg().cases == cases * repeats && chtest::agg().skipped == 8 * repeats, "parallel repeat/skip counts");
    expect(chtest::agg().checks == cases * repeats * 4 && chtest::agg().retries == cases * repeats, "parallel assertion/retry counts");
    expect(chtest::agg().retried_failures == cases * repeats && chtest::agg().failures == 0 && chtest::agg().failed_cases == 0,
           "one case's retry cannot erase another case's final failures");
    for (const auto& attempt : attempts) expect(attempt == repeats * 2, "each case repeats and retries exactly as requested");

    reset();
    for (int i = 0; i < 32; ++i) add("mixed final " + std::to_string(i), [i] {
        auto child = chtest::spawn_with_context([i] { CHECK_EQ(i % 4, 0); });
        child.join();
    }, 2);
    expect(run({"--threads", "8"}) == 1, "parallel permanent failures return failure");
    expect(chtest::agg().failed_cases == 24 && chtest::agg().failures == 24 && chtest::agg().retried_failures == 48,
           "parallel final and historical failures remain separate");
}
void sink_stress() {
    for (const auto limit : {1u, 31u, 256u, 65536u}) {
        reset();
        std::atomic<int> active{0};
        std::atomic<bool> serialized{true};
        std::size_t bytes_a = 0, bytes_b = 0;
        auto sink = [&](std::string_view text, std::size_t& bytes) {
            if (active.fetch_add(1) != 0) serialized = false;
            output.append(text);
            bytes += text.size();
            active.fetch_sub(1);
        };
        const chtest::output_sink_t first = [&](std::string_view text) { sink(text, bytes_a); };
        const chtest::output_sink_t second = [&](std::string_view text) { sink(text, bytes_b); };
        chtest::set_output_sink(first);
        Gate gate(9);
        {
            chtest::case_output_collector collector(limit);
            std::vector<std::thread> writers;
            for (int worker = 0; worker < 8; ++worker) writers.emplace_back(chtest::with_current_case_context([&, worker] {
                gate.arrive();
                for (int entry = 0; entry < 400; ++entry)
                    chtest::ts_cout() << "worker=" << worker << " entry=" << entry << '\n';
            }));
            std::thread replacer([&] {
                gate.arrive();
                for (int i = 0; i < 1000; ++i) chtest::set_output_sink(i % 2 ? first : second);
            });
            for (auto& writer : writers) writer.join();
            replacer.join();
        }
        expect(serialized && bytes_a + bytes_b == output.size(), "sink replacement preserves serialization and byte accounting");
        std::set<std::string> lines;
        std::istringstream received(output);
        std::string line;
        std::size_t line_count = 0;
        while (std::getline(received, line)) { lines.insert(line); ++line_count; }
        expect(line_count == 3200 && lines.size() == 3200, "concurrent output has no duplicate, split or lost messages");
        for (int worker = 0; worker < 8; ++worker)
            for (int entry = 0; entry < 400; ++entry)
                expect(lines.count("worker=" + std::to_string(worker) + " entry=" + std::to_string(entry)) == 1,
                       "each output message arrives exactly once");
    }
}
void vector_stress() {
    chtest::ThreadSafeVector<int> values;
    Gate gate(8);
    std::vector<std::thread> writers;
    for (int worker = 0; worker < 8; ++worker) writers.emplace_back([&, worker] {
        gate.arrive();
        for (int i = 0; i < 1000; ++i) {
            values.push_back(worker * 1000 + i);
            if (i % 100 == 0) (void)values.to_vector();
        }
    });
    for (auto& writer : writers) writer.join();
    auto result = values.to_vector();
    std::sort(result.begin(), result.end());
    expect(result.size() == 8000, "concurrent vector appends preserve size");
    for (int i = 0; i < 8000; ++i) expect(result[static_cast<std::size_t>(i)] == i, "concurrent vector append preserves every value");

    chtest::ThreadSafeVector<int> left, right;
    left.push_back(1); right.push_back(2);
    std::thread a([&] { for (int i = 0; i < 1000; ++i) left = right; });
    std::thread b([&] { for (int i = 0; i < 1000; ++i) right = left; });
    a.join(); b.join();
    expect(left.size() == 1 && right.size() == 1, "opposite-direction vector assignment does not deadlock");
}
void mock_stress() {
    chtest::MockFunction<int(int)> mock;
    mock.setHistoryLimit(37);
    mock.setImpl([](int value) { return value; });
    Gate gate(9);
    std::vector<std::thread> callers;
    std::atomic<bool> valid{true};
    for (int worker = 0; worker < 8; ++worker) callers.emplace_back([&, worker] {
        gate.arrive();
        for (int i = 0; i < 1000; ++i) {
            const int value = worker * 1000 + i;
            if (mock(value) != value) valid = false;
            if (i % 100 == 0) (void)mock.getCalls();
        }
    });
    std::thread configurer([&] {
        gate.arrive();
        for (int i = 0; i < 1000; ++i) {
            mock.setImpl([](int value) { return value; });
            mock.setHistoryLimit(static_cast<std::size_t>(i % 38));
        }
    });
    for (auto& caller : callers) caller.join();
    configurer.join();
    expect(valid && mock.timesCalled() == 8000, "mock calls survive concurrent implementation and history-limit changes");
    expect(mock.getCalls().size() <= 37, "concurrent mock history stays bounded");
}
void concurrent_run() {
    reset();
    std::promise<void> entered, release;
    auto ready = entered.get_future();
    auto released = release.get_future().share();
    add("held case", [&] { entered.set_value(); released.wait(); CHECK(true); });
    int first_result = -1;
    std::thread first([&] { first_result = run(); });
    const bool started = ready.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    char name[] = "competing runner";
    char* argv[]{name};
    const int second_result = started ? chtest::run(1, argv) : -1;
    release.set_value();
    first.join();
    expect(started && first_result == 0 && second_result == 2, "concurrent runner returns busy without corrupting active run");
    expect(chtest::agg().checks == 1 && chtest::agg().cases == 1, "busy runner does not reset active aggregate counters");
}
}
int main() {
    return verification::main("concurrency stress", [] {
        retry_stress();
        sink_stress();
        vector_stress();
        mock_stress();
        concurrent_run();
    });
}
