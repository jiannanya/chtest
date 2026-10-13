#include "runner_test_support.hpp"
#include <array>

namespace {
using namespace verification;
void filter_properties() {
    const auto oracle = [](std::string_view name, std::string_view pattern) {
        return pattern.empty() || std::search(name.begin(), name.end(), pattern.begin(), pattern.end(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        }) != name.end();
    };
    std::mt19937 rng(20261013);
    for (int i = 0; i < 20000; ++i) {
        std::string name(rng() % 129, '\0'), pattern(rng() % 40, '\0');
        for (auto& ch : name) ch = static_cast<char>(rng() % 256);
        for (auto& ch : pattern) ch = static_cast<char>(rng() % 256);
        if (i % 2 && !name.empty()) pattern = name.substr(rng() % name.size(), rng() % 40);
        if (i % 3 == 0) for (auto& ch : pattern) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        expect(chtest::FoldedNameFilter(pattern).matches(name) == oracle(name, pattern),
               "folded search matches the standard oracle including empty and high-byte strings");
    }
    for (const auto* name : {"", "a", "aaaaaab", "prefix NEEDLE suffix", "ababababababac"}) {
        for (const auto* pattern : {"", "A", "aaaab", "needle", "ABABAC", "much longer than the name"})
            expect(chtest::FoldedNameFilter(pattern).matches(name) == oracle(name, pattern), "overlapping search windows");
    }
    reset();
    int selected = 0;
    add("prefix NEEDLE suffix", [&] { ++selected; CHECK(true); });
    add("unselected", [] { CHECK(false); });
    expect(run({"--test", "needle"}) == 0 && selected == 1 && chtest::agg().cases == 1,
           "runner uses the prepared filter");
}
void deadline_rearming() {
    chtest::AttemptDeadline timer;
    std::atomic<bool> old_flag{false}, next_flag{false};
    timer.start(5000, old_flag); // start the persistent thread before the short probes
    expect(!timer.finish(), "unexpired deadline finishes cleanly");
    for (int i = 0; i < 1000; ++i) {
        timer.start(1, old_flag);
        timer.start(5000, next_flag);
        expect(!timer.finish() && !next_flag, "a superseded deadline cannot expire a later attempt");
    }
    timer.start(1, old_flag);
    timer.finish();
    timer.start(5000, next_flag);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expect(!timer.finish() && !next_flag, "canceled deadline stays canceled after its original expiry");
    old_flag = false;
    timer.start(5, old_flag);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!old_flag && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    expect(old_flag && timer.finish() && timer.finish(), "reused watchdog signals abort and finish is idempotent");
    next_flag = false;
    timer.start(0, next_flag);
    expect(!timer.finish() && !next_flag, "disabled deadline clears the previous expiration");
}
void timed_retries() {
    reset();
    std::array<int, 4> attempts{};
    for (std::size_t i = 0; i < attempts.size(); ++i) add("timed retry", [&, i] {
        CHECK(!chtest::tls_case_abort->load());
        if (++attempts[i] == 1) {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!chtest::tls_case_abort->load() && std::chrono::steady_clock::now() < limit)
                std::this_thread::yield();
        }
    }, 1);
    expect(run({"--threads", "4", "--timeout", "20"}) == 0, "timed-out attempts can retry on a reused watchdog");
    expect(chtest::agg().timeouts == 4 && chtest::agg().retries == 4 && chtest::agg().retried_failures == 4 &&
           chtest::agg().failures == 0 && chtest::agg().checks == 12, "timeout and retry accounting stays exact");
    reset();
    for (int i = 0; i < 128; ++i) add("short timed case", [] { CHECK(!chtest::tls_case_abort->load()); });
    expect(run({"--threads", "4", "--timeout", "5000", "--repeat", "3"}) == 0 &&
           chtest::agg().checks == 384 && chtest::agg().timeouts == 0, "watchdogs can serve many cases and repetitions");
    reset();
    std::vector<chtest::CaseContext> snapshots;
    add("retained context", [&] { snapshots.emplace_back(); CHECK(true); });
    expect(run({"--timeout", "5000", "--repeat", "64"}) == 0 && snapshots.size() == 64,
           "context snapshots can retain case signals through later cases");
    expect(!chtest::worker_deadline(), "a completed run releases its watchdog even when case snapshots remain alive");
    for (const auto& context : snapshots)
        expect(!context.abort->load(), "finished deadlines cannot signal retained case contexts");
}
}
int main() {
    return verification::main("performance regressions", [] {
        filter_properties();
        deadline_rearming();
        timed_retries();
    });
}
