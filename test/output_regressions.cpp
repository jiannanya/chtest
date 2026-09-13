#include "runner_test_support.hpp"
#include <future>

namespace {
using namespace verification;
struct BadFormatter {};
std::ostream& operator<<(std::ostream& stream, BadFormatter) {
    stream.setstate(std::ios::badbit);
    return stream;
}
void formatting_errors() {
    reset();
    add("bad output formatter", [] { chtest::ts_cout() << "prefix" << BadFormatter{} << "lost"; });
    expect(run() == 1 && chtest::output_failed(), "bad output formatter makes run fail");
    expect(output.find("prefix") != std::string::npos, "failed formatter preserves completed prefix");
    reset();
    add("next run recovers", [] { chtest::ts_cout() << 42; CHECK(true); });
    expect(run() == 0 && !chtest::output_failed(), "subsequent run clears output failure state");
}
void random_messages() {
    reset();
    std::mt19937 rng(20260912);
    for (int message = 0; message < 300; ++message) {
        output.clear();
        std::ostringstream reference;
        {
            auto actual = chtest::ts_cout();
            for (int part = 0; part < 24; ++part) {
                const auto size = static_cast<std::size_t>(rng() % 2049);
                const std::string text(size, static_cast<char>(rng() % 128));
                actual << std::string_view(text);
                reference << std::string_view(text);
                if (message % 3 && part == 12) {
                    actual << std::hex << std::showbase << std::setw(8) << std::setfill('_') << message;
                    reference << std::hex << std::showbase << std::setw(8) << std::setfill('_') << message;
                }
                actual << '\0'; reference << '\0';
            }
        }
        expect(output == reference.str(), "growing text and formatted streams match standard byte output");
    }
}
void chunk_boundaries() {
    for (const auto limit : {0u, 1u, 31u, 128u, 1024u}) {
        reset();
        std::string expected;
        {
            chtest::case_output_collector collector(limit);
            chtest::append_output(collector.buf.get(), std::string_view{});
            chtest::append_output(nullptr, std::string_view{});
            for (int i = 0; i < 400; ++i) {
                const auto size = static_cast<std::size_t>(i % 5 == 0 ? limit + 1 : i % 129);
                const std::string text(size, static_cast<char>('a' + i % 26));
                chtest::append_output(collector.buf.get(), text);
                expected += text;
                expect(collector.buf->buf.size() <= limit, "buffer payload respects configured limit");
            }
        }
        expect(output == expected, "small, exact-limit, oversized and empty messages preserve serial order");
    }
}
void concurrent_refill() {
    // Hold a flushing callback while another producer fills the emptied buffer.
    // The pending message must recheck capacity before appending.
    reset();
    std::promise<void> entered, release;
    auto ready = entered.get_future();
    auto released = release.get_future().share();
    std::atomic<bool> first{true};
    chtest::set_output_sink([&](std::string_view text) {
        output.append(text);
        if (first.exchange(false)) { entered.set_value(); released.wait(); }
    });
    auto buffer = std::make_shared<chtest::PerCaseBuffer>();
    buffer->limit = 8;
    chtest::append_output(buffer.get(), "aaaaaa");
    std::thread flusher([&] { chtest::append_output(buffer.get(), "bbb"); });
    const bool started = ready.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    if (started) chtest::append_output(buffer.get(), "cccccc");
    release.set_value();
    flusher.join();
    expect(started, "flush reached sink callback");
    chtest::append_output(buffer.get(), "dddddddd");
    expect(output == "aaaaaaccccccbbbdddddddd", "concurrent refill rechecks capacity without overwriting a producer");
    expect(buffer->buf.empty(), "oversized message drains pending buffered data");
}
void reentrant_close() {
    reset();
    auto buffer = std::make_shared<chtest::PerCaseBuffer>();
    buffer->limit = 8;
    chtest::set_output_sink([&](std::string_view text) {
        output.append(text);
        std::lock_guard<std::mutex> lock(buffer->mtx);
        buffer->closed = true;
    });
    chtest::append_output(buffer.get(), "123456");
    chtest::append_output(buffer.get(), "789");
    expect(output == "123456789" && buffer->buf.empty(), "closing during flush forwards the pending message");
    reset();
}
}
int main() {
    return verification::main("output regressions", [] {
        formatting_errors();
        random_messages();
        chunk_boundaries();
        concurrent_refill();
        reentrant_close();
    });
}
