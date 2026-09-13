#include "runner_test_support.hpp"
#include <cstdlib>
#include <cstddef>
#include <new>

// Isolated executable: track C++ allocations, including aligned new, without
// allocating in the tracker itself. This is not a substitute for ASan/LSan.
namespace allocation_probe {
struct Header { void* base; std::size_t bytes; bool tracked; };
inline thread_local bool enabled = false;
inline thread_local std::size_t large_threshold = 1024;
inline std::atomic<std::size_t> allocations{0}, large_allocations{0}, live{0}, peak{0};
void* allocate(std::size_t bytes, std::size_t alignment) {
    alignment = std::max(alignment, alignof(Header));
    const auto overhead = sizeof(Header) + alignment - 1;
    if (bytes > std::numeric_limits<std::size_t>::max() - overhead) throw std::bad_alloc();
    void* base = std::malloc(std::max<std::size_t>(bytes, 1) + overhead);
    if (!base) throw std::bad_alloc();
    const auto begin = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto address = (begin + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    auto* header = reinterpret_cast<Header*>(address) - 1;
    ::new (header) Header{base, bytes, enabled};
    if (enabled) {
        allocations.fetch_add(1);
        if (bytes >= large_threshold) large_allocations.fetch_add(1);
        const auto current = live.fetch_add(bytes) + bytes;
        auto before = peak.load();
        while (current > before && !peak.compare_exchange_weak(before, current)) {}
    }
    return reinterpret_cast<void*>(address);
}
void release(void* pointer) noexcept {
    if (!pointer) return;
    auto* header = reinterpret_cast<Header*>(pointer) - 1;
    if (header->tracked) live.fetch_sub(header->bytes);
    std::free(header->base);
}
struct Scope {
    explicit Scope(std::size_t threshold = 1024) {
        allocations = 0; large_allocations = 0; peak = live.load(); large_threshold = threshold; enabled = true;
    }
    ~Scope() { enabled = false; }
};
}
void* operator new(std::size_t bytes) { return allocation_probe::allocate(bytes, alignof(std::max_align_t)); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer) noexcept { allocation_probe::release(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment) { return allocation_probe::allocate(bytes, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t bytes, std::align_val_t alignment) { return ::operator new(bytes, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { allocation_probe::release(pointer); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { try { return ::operator new(bytes); } catch (...) { return nullptr; } }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { try { return ::operator new[](bytes); } catch (...) { return nullptr; } }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { allocation_probe::release(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { allocation_probe::release(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { allocation_probe::release(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { allocation_probe::release(pointer); }
#endif

namespace {
using verification::expect;
void fast_paths() {
    chtest::ContextRestore restore;
    chtest::route().mode = chtest::SubcaseMode::Discovery;
    chtest::current_quiet() = true;
    chtest::reset_aggregates();
    CHECK_EQ(1, 1); // initialize lazy runtime state outside the measured region
    {
        allocation_probe::Scope scope;
        for (int i = 0; i < 100000; ++i) CHECK_EQ(i, i);
    }
    expect(allocation_probe::allocations == 0, "quiet successful assertions allocate no heap memory");
    expect(chtest::agg().checks == 100001, "allocation probe executes all assertions");

    std::size_t bytes = 0;
    chtest::set_output_sink([&](std::string_view text) { bytes += text.size(); });
    chtest::ts_cout() << "warmup";
    bytes = 0;
    {
        allocation_probe::Scope scope;
        for (int i = 0; i < 1000; ++i) chtest::ts_cout() << "short message" << '\n';
    }
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
    expect(bytes == 14000 && allocation_probe::allocations <= 1000, "short text needs at most one standard-library debug proxy per message");
#else
    expect(bytes == 14000 && allocation_probe::allocations == 0, "inline text output allocates no heap memory");
#endif
    chtest::clear_output_sink();
}
void mock_memory() {
    chtest::MockFunction<void(int)> mock;
    mock.setRecordCalls(false);
    {
        allocation_probe::Scope scope;
        mock.reserveCalls(1000000);
        for (int i = 0; i < 10000; ++i) mock(i);
    }
    expect(allocation_probe::allocations == 0 && mock.timesCalled() == 10000, "count-only mock ignores reserve and allocates no history");
    mock.setRecordCalls(true);
    mock.setHistoryLimit(256);
    mock.reserveCalls(256);
    for (int i = 0; i < 16; ++i) mock(i);
    {
        allocation_probe::Scope scope;
        mock.setHistoryLimit(512);
        for (int i = 16; i < 256; ++i) mock(i);
    }
    expect(allocation_probe::allocations == 0, "raising history limit preserves reserved storage");
    mock.reset(true);
    {
        allocation_probe::Scope scope;
        {
            chtest::MockFunction<void(const std::string&)> bounded;
            bounded.setHistoryLimit(4);
            const std::string payload(4096, 'x');
            for (int i = 0; i < 20000; ++i) bounded(payload);
            expect(bounded.timesCalled() == 20000 && bounded.getCalls().size() == 4, "bounded history still counts all calls");
        }
    }
    expect(allocation_probe::peak < 128 * 1024, "bounded argument history stays below a fixed allocation budget");
    expect(allocation_probe::live == 0, "mock destruction releases tracked history and payloads");
}
void buffer_reuse() {
    std::size_t received = 0;
    chtest::set_output_sink([&](std::string_view text) { received += text.size(); });
    const std::string message(128, 'x');
    {
        allocation_probe::Scope scope;
        {
            chtest::case_output_collector collector(1024);
            for (int i = 0; i < 10000; ++i) chtest::append_output(collector.buf.get(), message);
        }
    }
    std::cout << "buffer payload allocations=" << allocation_probe::large_allocations << '\n';
    expect(received == 1280000, "buffer reuse emits every byte");
    expect(allocation_probe::large_allocations < 16, "serial chunk flushes reuse allocated payload storage");
    expect(allocation_probe::peak < 8192, "reusable buffer keeps a bounded allocation peak");
    expect(allocation_probe::live == 0, "collector destruction releases reusable payload storage");
    chtest::clear_output_sink();
}
void formatted_snapshot() {
    std::size_t received = 0;
    chtest::set_output_sink([&](std::string_view text) { received += text.size(); });
    const std::string payload(8192, 'x');
    auto message = std::make_unique<chtest::ts_ostream_proxy>();
    *message << 42 << ':' << payload;
    {
        allocation_probe::Scope scope;
        message.reset();
    }
    expect(received == 8195, "formatted snapshot preserves full output");
#if (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L) || __cplusplus >= 202002L
    expect(allocation_probe::large_allocations == 0, "C++20 formatted output uses the stream view without a payload copy");
#endif
    chtest::clear_output_sink();
}
void replay_storage() {
    verification::reset();
    std::vector<std::string> names;
    for (int i = 0; i < 16; ++i) names.push_back(std::string(8192, 'x') + std::to_string(i));
    verification::add("long paths", [&] {
        for (const auto& name : names) SUBCASE(name.c_str()) { CHECK(true); }
    });
    chtest::set_output_sink([](std::string_view) {});
    int result;
    {
        allocation_probe::Scope scope(8192);
        result = verification::run();
    }
    expect(result == 0 && chtest::agg().subcases == 16, "long paths replay correctly");
    expect(allocation_probe::large_allocations <= 64, "replay does not copy the leaf display name twice");
    verification::reset();
    chtest::route() = chtest::RouteState{};
}
void repeated_runs() {
    verification::reset();
    verification::add("dynamic path retention", [] {
        for (int i = 0; i < 64; ++i) {
            const auto name = "dynamic allocation test subcase with a long name " + std::to_string(i);
            SUBCASE(name.c_str()) { CHECK(true); }
        }
    });
    chtest::set_output_sink([](std::string_view) {});
    expect(verification::run() == 0, "warmup run");
    chtest::reset_aggregates();
    std::size_t retained = 0;
    for (int i = 0; i < 40; ++i) {
        int result;
        {
            allocation_probe::Scope scope;
            result = verification::run(i % 2 ? std::initializer_list<const char*>{"--timings"} : std::initializer_list<const char*>{});
            chtest::reset_aggregates();
        }
        expect(result == 0, "repeated timed/untimed run succeeds");
        const auto live = allocation_probe::live.load();
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
        // MSVC keeps a small debug iterator proxy even in empty global vectors.
        expect(live <= 512 && (i == 0 || live == retained), "only fixed-size debug iterator metadata remains after a run");
#else
        expect(live == 0, "repeated run releases paths, context, indices and timing storage");
#endif
        retained = live;
        expect(chtest::registry()[0].subcases.capacity() == 0, "registry does not retain discovered paths");
    }
    std::cout << "retained run metadata bytes=" << retained << '\n';
}
void sparse_filters() {
    verification::reset();
    for (int i = 0; i < 4096; ++i)
        verification::add("unselected case " + std::to_string(i), [] { CHECK(true); });
    verification::add("needle", [] { CHECK(true); });
    chtest::set_output_sink([](std::string_view) {});
    expect(verification::run({"--test", "needle"}) == 0, "warm up filtered runner");
    {
        allocation_probe::Scope scope;
        expect(verification::run({"--test", "needle"}) == 0 && chtest::agg().cases == 1,
               "sparse filter selects exactly the matching case");
    }
    expect(allocation_probe::peak < 8192, "sparse filter does not reserve indices for the entire registry");
    {
        allocation_probe::Scope scope;
        expect(verification::run({"--test", "absent"}) == 0 && chtest::agg().cases == 0,
               "empty selection executes no cases");
    }
    expect(allocation_probe::peak < 8192, "empty selection does not allocate a registry-sized index");
}
}
int main() {
    return verification::main("allocation contracts", [] {
        fast_paths();
        buffer_reuse();
        formatted_snapshot();
        replay_storage();
        mock_memory();
        repeated_runs();
        sparse_filters();
    });
}
