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
void numeric_messages() {
    reset();
    const long long signed_values[] = {0, 1, -1, 7, 42, 1000, 1234567890, -2147483648LL, 2147483647LL,
                                       (-9223372036854775807LL - 1), 9223372036854775807LL};
    std::string expected;
    {
        auto actual = chtest::ts_cout();
        std::ostringstream reference;
        for (const long long value : signed_values) {
            actual << value << ' ';
            reference << value << ' ';
            actual << static_cast<int>(value) << ' ' << static_cast<short>(value) << ' ';
            reference << static_cast<int>(value) << ' ' << static_cast<short>(value) << ' ';
            actual << static_cast<unsigned int>(value) << ' ' << static_cast<unsigned long long>(value) << ' ';
            reference << static_cast<unsigned int>(value) << ' ' << static_cast<unsigned long long>(value) << ' ';
        }
        actual << static_cast<unsigned short>(65535) << ' ' << static_cast<unsigned>(4294967295U) << ' '
               << static_cast<unsigned long long>(18446744073709551615ULL);
        reference << static_cast<unsigned short>(65535) << ' ' << 4294967295U << ' ' << 18446744073709551615ULL;
        expected = reference.str();
    }
    expect(output == expected, "direct integer spelling matches the standard stream byte for byte");
    // A following operand of any other type still switches the whole message to
    // the stream without losing or reordering the digits already spelled.
    reset();
    {
        auto actual = chtest::ts_cout();
        std::ostringstream reference;
        actual << 17 << " mixed " << 3.5 << ' ' << true;
        reference << 17 << " mixed " << 3.5 << ' ' << true;
        expected = reference.str();
    }
    expect(output == expected, "a non-integer operand keeps stream semantics after direct digits");
    // Manipulators still take over the message and keep standard formatting.
    reset();
    {
        auto actual = chtest::ts_cout();
        std::ostringstream reference;
        actual << 42 << std::hex << std::showbase << std::setw(6) << std::setfill('_') << 255;
        reference << 42 << std::hex << std::showbase << std::setw(6) << std::setfill('_') << 255;
        expected = reference.str();
    }
    expect(output == expected, "manipulators after direct digits keep standard formatting");
    reset();
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
void direct_large_messages() {
    reset();
    const std::string large(chtest::direct_message_threshold + 37, 'L');
    auto buffer = std::make_shared<chtest::PerCaseBuffer>();
    buffer->limit = 64; // pending content must be flushed before a direct message
    chtest::append_output(buffer.get(), "pending-");
    chtest::append_output(buffer.get(), large);
    expect(output == "pending-" + large, "a direct message flushes pending chunks first and keeps order");
    expect(buffer->buf.empty(), "a direct message leaves no buffered payload behind");
    chtest::append_output(buffer.get(), large);
    expect(output == "pending-" + large + large, "consecutive direct messages keep their order");
    expect(buffer->buf.empty() && output.size() == 8 + 2 * large.size(), "direct messages emit every byte once");
    reset();
}
void concurrent_large_messages() {
    reset();
    const std::size_t size = chtest::direct_message_threshold + 11;
    auto buffer = std::make_shared<chtest::PerCaseBuffer>();
    buffer->limit = 64; // every message takes the direct path
    std::vector<std::string> written;
    for (int writer = 0; writer < 4; ++writer) {
        for (int index = 0; index < 25; ++index) {
            std::string message(size, static_cast<char>('a' + writer));
            const std::string tag = std::to_string(writer) + ":" + std::to_string(index) + ";";
            std::copy(tag.begin(), tag.end(), message.begin());
            written.push_back(message);
        }
    }
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 4; ++writer) {
        writers.emplace_back([&, writer] {
            for (int index = 0; index < 25; ++index) chtest::append_output(buffer.get(), written[writer * 25 + index]);
        });
    }
    for (auto& thread : writers) thread.join();
    expect(output.size() == written.size() * size, "every concurrent direct message reached the sink");
    // Splitting the emission into message-sized blocks must yield only whole
    // messages: a cut or an interleaved byte would break a block.
    bool whole = true;
    for (std::size_t block = 0; block * size < output.size(); ++block) {
        const std::string_view part(output.data() + block * size, size);
        whole = whole && std::find(written.begin(), written.end(), part) != written.end();
    }
    expect(whole, "concurrent direct messages are emitted whole, never interleaved");
    expect(buffer->buf.empty(), "concurrent direct messages leave the buffer empty");
    reset();
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
        numeric_messages();
        chunk_boundaries();
        direct_large_messages();
        concurrent_refill();
        concurrent_large_messages();
        reentrant_close();
    });
}
