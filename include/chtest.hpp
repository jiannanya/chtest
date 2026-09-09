#ifndef CH_TEST_HPP
#define CH_TEST_HPP

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <charconv>
#include <condition_variable>
#include <cctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <cmath>
#include <iterator>
#include <string_view>

namespace chtest {

using count_type = std::uint64_t;

// Output is buffered in bounded chunks; context helpers keep buffers alive.
struct PerCaseBuffer {
    std::string buf;
    std::mutex mtx;
    std::size_t limit = 64 * 1024;
    bool closed = false;
};
inline thread_local PerCaseBuffer* tls_case_out = nullptr;
inline thread_local std::shared_ptr<PerCaseBuffer> tls_case_out_keep;
inline thread_local std::shared_ptr<std::atomic<bool>> tls_case_abort_keep;
inline thread_local std::atomic<bool>* tls_case_abort = nullptr;
inline thread_local std::shared_ptr<std::atomic<count_type>> tls_case_fail_count_keep;
inline thread_local std::atomic<count_type>* tls_case_fail_count = nullptr;
inline thread_local bool tls_in_output_sink = false;
inline std::mutex& global_out_mutex() { static std::mutex m; return m; }
inline std::atomic<bool>& output_failed() { static std::atomic<bool> failed{false}; return failed; }

using output_sink_t = std::function<void(std::string_view)>;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
inline std::atomic<std::shared_ptr<output_sink_t>>& output_sink() {
    static std::atomic<std::shared_ptr<output_sink_t>> sink;
    return sink;
}
inline std::shared_ptr<output_sink_t> load_output_sink() { return output_sink().load(std::memory_order_acquire); }
#else
inline std::shared_ptr<output_sink_t>& output_sink() {
    static std::shared_ptr<output_sink_t> sink;
    return sink;
}
inline std::shared_ptr<output_sink_t> load_output_sink() {
    return std::atomic_load_explicit(&output_sink(), std::memory_order_acquire);
}
#endif
inline void set_output_sink(output_sink_t sink) {
    auto next = sink ? std::make_shared<output_sink_t>(std::move(sink)) : std::shared_ptr<output_sink_t>{};
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    output_sink().store(std::move(next), std::memory_order_release);
#else
    std::atomic_store_explicit(&output_sink(), std::move(next), std::memory_order_release);
#endif
}
inline void clear_output_sink() { set_output_sink({}); }
inline void emit_output(std::string_view s) noexcept {
    if (s.empty()) return;
    // Recursive serialization permits a sink to log through ts_cout(). Nested
    // output goes to stdout so a sink cannot recursively call itself forever.
    static std::recursive_mutex mutex;
    try {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        auto sink = load_output_sink();
        if (sink && !tls_in_output_sink) {
            struct Guard { bool& flag; Guard(bool& f) : flag(f) { flag = true; } ~Guard() { flag = false; } } guard(tls_in_output_sink);
            (*sink)(s);
        } else {
            std::cout.write(s.data(), static_cast<std::streamsize>(s.size()));
            if (!std::cout) output_failed().store(true, std::memory_order_relaxed);
        }
    } catch (...) { output_failed().store(true, std::memory_order_relaxed); }
}
inline void append_output(PerCaseBuffer* buffer, std::string_view text) {
    if (!buffer || tls_in_output_sink) { emit_output(text); return; }
    std::unique_lock<std::mutex> lock(buffer->mtx);
    if (!buffer->closed && text.size() < buffer->limit &&
        buffer->buf.size() <= buffer->limit - text.size()) {
        buffer->buf.append(text.data(), text.size());
        return;
    }
    std::string ready;
    const bool direct = buffer->closed || text.size() >= buffer->limit;
    ready.swap(buffer->buf);
    if (!direct) buffer->buf.append(text.data(), text.size());
    lock.unlock();
    // Never invoke user callbacks while holding the buffer mutex.
    if (!ready.empty()) emit_output(ready);
    if (direct) emit_output(text);
}
class ts_ostream_proxy {
    char small_text[256];
    std::size_t small_size = 0;
    std::string text;
    std::optional<std::ostringstream> stream;
    std::string_view text_view() const {
        return text.empty() ? std::string_view(small_text, small_size) : std::string_view(text);
    }
    void append_text(std::string_view value) {
        if (value.empty()) return;
        if (text.empty() && value.size() <= sizeof(small_text) - small_size) {
            std::char_traits<char>::copy(small_text + small_size, value.data(), value.size());
            small_size += value.size();
        } else {
            if (text.empty()) {
                text.reserve(small_size + value.size());
                text.append(small_text, small_size);
                small_size = 0;
            }
            text.append(value.data(), value.size());
        }
    }
    std::ostringstream& formatted() {
        if (!stream) {
            stream.emplace();
            const auto prefix = text_view();
            if (!prefix.empty()) stream->write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
            std::string().swap(text);
            small_size = 0;
        }
        return *stream;
    }
public:
    // Text-only messages need no locale, stream buffer, or final str() copy.
    ts_ostream_proxy& operator<<(std::string_view value) {
        if (stream) *stream << value;
        else append_text(value);
        return *this;
    }
    ts_ostream_proxy& operator<<(const std::string& value) { return *this << std::string_view(value); }
    ts_ostream_proxy& operator<<(const char* value) {
        if (!value) formatted() << value;
        else *this << std::string_view(value);
        return *this;
    }
    ts_ostream_proxy& operator<<(char value) {
        if (stream) *stream << value;
        else append_text(std::string_view(&value, 1));
        return *this;
    }
    template <typename T> ts_ostream_proxy& operator<<(const T& value) { formatted() << value; return *this; }
    ts_ostream_proxy& operator<<(std::ostream& (*manip)(std::ostream&)) { formatted() << manip; return *this; }
    ~ts_ostream_proxy() noexcept {
        try {
            if (stream) append_output(tls_case_out, stream->str());
            else append_output(tls_case_out, text_view());
        }
        catch (...) { output_failed().store(true, std::memory_order_relaxed); }
    }
};
inline ts_ostream_proxy ts_cout() { return ts_ostream_proxy(); }
struct case_output_collector {
    std::shared_ptr<PerCaseBuffer> buf;
    PerCaseBuffer* previous;
    std::shared_ptr<PerCaseBuffer> previous_keep;
    explicit case_output_collector(std::size_t limit = 64 * 1024)
        : case_output_collector(std::make_shared<PerCaseBuffer>(), limit) {}
    case_output_collector(std::shared_ptr<PerCaseBuffer> buffer, std::size_t limit)
        : buf(std::move(buffer)), previous(tls_case_out), previous_keep(tls_case_out_keep) {
        buf->limit = limit;
        tls_case_out_keep = buf;
        tls_case_out = buf.get();
    }
    ~case_output_collector() noexcept {
        tls_case_out = previous;
        tls_case_out_keep = std::move(previous_keep);
        try {
            std::string text;
            { std::lock_guard<std::mutex> lock(buf->mtx); buf->closed = true; text.swap(buf->buf); }
            append_output(previous, text);
        } catch (...) { output_failed().store(true, std::memory_order_relaxed); }
    }
    case_output_collector(const case_output_collector&) = delete;
    case_output_collector& operator=(const case_output_collector&) = delete;
};

// ---------- Unique name helpers ----------
#define CH_TEST_CONCAT_INNER(a,b) a##b
#define CH_TEST_CONCAT(a,b) CH_TEST_CONCAT_INNER(a,b)
#define CH_TEST_UNIQUE_NAME(base) CH_TEST_CONCAT(base, __COUNTER__)

// ---------- Config ----------
struct Config {
    std::string pattern;
    bool help = false;
    bool timings = false;
    std::size_t buffer_limit = 64 * 1024;
    std::vector<std::string> any_tags, all_tags, excluded_tags;
    bool untagged_only = false;
    bool list_all = false;      // --list
    bool list_cases = false;    // --cases
    int repeat = 1;             // --repeat N
    bool shuffle = false;       // --shuffle <seed>
    unsigned seed = 0;
    bool quiet = false;         // --quiet
    bool no_color = false;      // --no-color
    bool no_buffer = false;     // --no-buffer
    int timeout_ms = 0;         // --timeout
    int threads = 1;            // --threads N
    int default_retries = 0;    // --retries N (global fallback)
    int slow_ms = 0;            // --slow-threshold N (ms) - mark cases slower than this
    enum class TagMode { None, Any, All, NoTag, NotAny } tag_mode = TagMode::None;
    std::vector<std::string> tags; // tags to filter on (mode-dependent)
};

// ---------- Color ----------
struct Color {
    bool enabled = true;
    static Color& instance() { static Color c; return c; }
    void set_enabled(bool e) { enabled = e; }
    // return raw ANSI codes so callers can include them in a single ts_cout() expression
    const char* red_code()    const { return "\033[31m"; }
    const char* green_code()  const { return "\033[32m"; }
    const char* yellow_code() const { return "\033[33m"; }
    const char* blue_code()   const { return "\033[34m"; }
    const char* dim_code()    const { return "\033[2m"; }
    const char* reset_code()  const { return "\033[0m"; }

    // backwards-compatible void methods: still emit via ts_cout()
    void red()   { if(enabled) ts_cout() << red_code(); }
    void green() { if(enabled) ts_cout() << green_code(); }
    void yellow(){ if(enabled) ts_cout() << yellow_code(); }
    void blue()  { if(enabled) ts_cout() << blue_code(); }
    void dim()   { if(enabled) ts_cout() << dim_code(); }
    void reset() { if(enabled) ts_cout() << reset_code(); }
};


// ---------- Environment ----------
struct Environment {
    virtual ~Environment() = default;
    virtual void setUp() {}
    virtual void tearDown() {}
};
inline Environment*& global_env() { static Environment* env=nullptr; return env; }

#define CHTEST_SET_ENV(ENVOBJ) ::chtest::global_env() = &(ENVOBJ);


// ---------- Test case & registry ----------
struct SubcaseId {
    std::string name;
    const char* file = "";
    int line = 0;
    bool operator==(const SubcaseId& other) const {
        return line == other.line && name == other.name && std::string_view(file) == other.file;
    }
};
struct Subcase {
    std::string name;
    std::vector<SubcaseId> path;
};

struct TestCase {
    std::string name;
    std::function<void()> fn;
    std::vector<Subcase> subcases; // collected in discovery
    bool is_fixture = false;
    std::vector<std::string> tags; // optional tags for filtering
    int priority = 0; // higher runs first
    int retries = 0;  // per-case retry count (number of retries after first attempt)
    std::function<bool()> skip_if = nullptr; // dynamic runtime skip predicate
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> R;
    return R;
}

struct TestRegistrar {
    // Backwards-compatible constructor: existing macros still work.
    TestRegistrar(const char* name,
                  std::function<void()> fn,
                  bool fixture = false,
                  std::vector<std::string> tags = {},
                  int priority = 0,
                  int retries = 0,
                  std::function<bool()> skip_if = nullptr) {
        registry().push_back({name, std::move(fn), {}, fixture, std::move(tags), priority, retries, std::move(skip_if)});
    }
};

// Convenience registration macros that accept priority/retries/skip predicate
#define TEST_CASE_PRIO(NAME, PRIO) TEST_CASE_PRIORITY(NAME, PRIO)

// Helper macros to register with explicit priority or retries or skip predicate
#define TEST_CASE_WITH_OPTS_IMPL(NAME, PRIO, RETRIES, SKIP_PRED, CH_TEST_FN, CH_TEST_REG) \
    static void CH_TEST_FN(); \
    static ::chtest::TestRegistrar CH_TEST_REG{ NAME, CH_TEST_FN, false, std::vector<std::string>(), PRIO, RETRIES, SKIP_PRED }; \
    static void CH_TEST_FN()

#define TEST_CASE_WITH_OPTS(NAME, PRIO, RETRIES, SKIP_PRED) \
TEST_CASE_WITH_OPTS_IMPL(NAME, PRIO, RETRIES, SKIP_PRED, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG))



#define TEST_CASE_PRIORITY_IMPL(NAME, PRIO, CH_TEST_FN, CH_TEST_REG) \
    static void CH_TEST_FN(); \
    static ::chtest::TestRegistrar CH_TEST_REG{ NAME, CH_TEST_FN, false, std::vector<std::string>(), PRIO, 0, nullptr }; \
    static void CH_TEST_FN()

#define TEST_CASE_PRIORITY(NAME, PRIO) \
TEST_CASE_PRIORITY_IMPL(NAME, PRIO, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG))



#define TEST_CASE_RETRY_IMPL(NAME, N, CH_TEST_FN, CH_TEST_REG) \
    static void CH_TEST_FN(); \
    static ::chtest::TestRegistrar CH_TEST_REG{ NAME, CH_TEST_FN, false, std::vector<std::string>(), 0, N, nullptr }; \
    static void CH_TEST_FN()

#define TEST_CASE_RETRY(NAME, N) \
TEST_CASE_RETRY_IMPL(NAME, N, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG))



#define TEST_CASE_SKIP_IF_IMPL(NAME, PRED,CH_TEST_FN, CH_TEST_REG) \
    static void CH_TEST_FN(); \
    static ::chtest::TestRegistrar CH_TEST_REG{ NAME, CH_TEST_FN, false, std::vector<std::string>(), 0, 0, std::function<bool()>([](){ return (PRED); }) }; \
    static void CH_TEST_FN()

#define TEST_CASE_SKIP_IF(NAME, PRED) \
TEST_CASE_SKIP_IF_IMPL(NAME, PRED, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG))


// ---------- Global subcase routing state ----------
enum class SubcaseMode { Normal, Discovery, Active };
using SubcaseIndex = std::unordered_multimap<std::size_t, std::size_t>;

struct RouteState {
    SubcaseMode mode = SubcaseMode::Normal;
    const char* active_name = nullptr;
    TestCase* current_case = nullptr;
    bool in_subcase = false;
    const std::vector<SubcaseId>* active_path = nullptr;
    std::vector<SubcaseId> path;
    SubcaseIndex* discovery_index = nullptr;
    std::size_t entered_depth = 0;
};
inline RouteState& route() { static thread_local RouteState state; return state; }
inline bool assertions_enabled() {
    const auto& rt = route();
    return rt.mode == SubcaseMode::Discovery ||
        (rt.mode == SubcaseMode::Active && rt.in_subcase &&
         (!rt.active_path || rt.path.size() == rt.active_path->size()));
}
inline std::size_t subcase_hash_append(std::size_t hash, std::string_view name, std::string_view file, int line) {
    const auto combine = [&](std::size_t value) { hash ^= value + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2); };
    combine(std::hash<std::string_view>{}(name));
    combine(std::hash<std::string_view>{}(file));
    combine(static_cast<std::size_t>(line));
    return hash;
}
inline std::size_t subcase_path_hash(const std::vector<SubcaseId>& path) {
    std::size_t hash = 0;
    for (const auto& id : path) hash = subcase_hash_append(hash, id.name, id.file, id.line);
    return hash;
}
inline bool subcase_enter(const char* name, const char* file = "", int line = 0) {
    auto& rt = route();
    const auto matches = [&](const SubcaseId& id) {
        return id.line == line && std::string_view(id.name) == name &&
               (id.file == file || std::string_view(id.file) == file);
    };
    if (rt.mode == SubcaseMode::Active && rt.active_path && rt.path.size() < rt.active_path->size()) {
        if (rt.entered_depth > rt.path.size() || !matches((*rt.active_path)[rt.path.size()])) return false;
        rt.entered_depth = rt.path.size() + 1;
        return true;
    }
    if (rt.current_case && (rt.mode == SubcaseMode::Discovery ||
                           (rt.mode == SubcaseMode::Active && rt.in_subcase))) {
        auto& subcases = rt.current_case->subcases;
        const auto same_path = [&](const Subcase& sc) {
            return sc.path.size() == rt.path.size() + 1 && matches(sc.path.back()) &&
                   std::equal(rt.path.begin(), rt.path.end(), sc.path.begin());
        };
        // Small cases keep the allocation-free linear path. Large discoveries
        // use an index of vector positions, with full equality on hash collisions.
        auto* index = rt.discovery_index;
        const bool indexed = index && subcases.size() >= 32;
        std::size_t hash = 0;
        if (indexed) {
            if (index->empty()) {
                index->reserve(subcases.size() * 2);
                for (std::size_t i = 0; i < subcases.size(); ++i)
                    index->emplace(subcase_path_hash(subcases[i].path), i);
            }
            hash = subcase_hash_append(subcase_path_hash(rt.path), name, file, line);
            const auto range = index->equal_range(hash);
            for (auto found = range.first; found != range.second; ++found)
                if (same_path(subcases[found->second])) return false;
        } else if (std::any_of(subcases.begin(), subcases.end(), same_path)) return false;
        auto path = rt.path;
        path.push_back({name, file, line});
        subcases.push_back({name, std::move(path)});
        if (indexed) index->emplace(hash, subcases.size() - 1);
    }
    return rt.mode == SubcaseMode::Active && !rt.active_path && rt.active_name &&
           std::string_view(rt.active_name) == name;
}
struct ScopedSubcaseFlag {
    bool active = true;
    bool previous = route().in_subcase;
    bool pushed = false;
    ScopedSubcaseFlag() { route().in_subcase = true; }
    ScopedSubcaseFlag(const char* name, const char* file, int line) : active(subcase_enter(name, file, line)) {
        if (active) {
            route().path.push_back({name, file, line});
            route().in_subcase = true;
            pushed = true;
        }
    }
    ~ScopedSubcaseFlag() {
        if (pushed) route().path.pop_back();
        route().in_subcase = previous;
    }
};
struct AssertionFailure : std::exception {
    const char* what() const noexcept override { return "REQUIRE failed"; }
};
template <typename F> bool invoke_guarded(F&& fn, const char* description);

// A full RAII snapshot also restores the per-case failure counter on exceptions.
struct CaseContext {
    RouteState routing = route();
    PerCaseBuffer* out = tls_case_out;
    std::shared_ptr<PerCaseBuffer> out_keep = tls_case_out_keep;
    std::atomic<bool>* abort = tls_case_abort;
    std::shared_ptr<std::atomic<bool>> abort_keep = tls_case_abort_keep;
    std::atomic<count_type>* failures = tls_case_fail_count;
    std::shared_ptr<std::atomic<count_type>> failures_keep = tls_case_fail_count_keep;
    void install() const {
        route() = routing;
        tls_case_out = out; tls_case_out_keep = out_keep;
        tls_case_abort = abort; tls_case_abort_keep = abort_keep;
        tls_case_fail_count = failures; tls_case_fail_count_keep = failures_keep;
    }
    void restore() noexcept {
        route() = std::move(routing);
        tls_case_out = out; tls_case_out_keep = std::move(out_keep);
        tls_case_abort = abort; tls_case_abort_keep = std::move(abort_keep);
        tls_case_fail_count = failures; tls_case_fail_count_keep = std::move(failures_keep);
    }
};
struct ContextRestore {
    CaseContext previous;
    ~ContextRestore() { previous.restore(); }
};
template <typename F>
inline auto with_current_case_context(F&& f, std::shared_ptr<std::atomic<bool>> abort_keep = nullptr) {
    CaseContext context;
    // Subcases may only be registered by the case runner, never by a child.
    context.routing.current_case = nullptr;
    context.routing.discovery_index = nullptr;
    if (abort_keep) { context.abort_keep = std::move(abort_keep); context.abort = context.abort_keep.get(); }
    return [context = std::move(context), fn = std::forward<F>(f)]() mutable -> decltype(auto) {
        ContextRestore guard;
        context.install();
        return std::invoke(fn);
    };
}
inline std::shared_ptr<std::atomic<bool>> make_case_abort() {
    return std::make_shared<std::atomic<bool>>(false);
}
template <typename Fn, typename... Args>
inline std::thread spawn_with_context(std::shared_ptr<std::atomic<bool>> abort_keep, Fn&& fn, Args&&... args) {
    auto task = [fn = std::forward<Fn>(fn), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        invoke_guarded([&] { std::apply(std::move(fn), std::move(args)); }, "uncaught exception in child thread");
    };
    return std::thread(with_current_case_context(std::move(task), std::move(abort_keep)));
}
template <typename Fn, typename... Args,
          std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, std::shared_ptr<std::atomic<bool>>>, int> = 0>
inline std::thread spawn_with_context(Fn&& fn, Args&&... args) {
    return spawn_with_context(std::shared_ptr<std::atomic<bool>>{}, std::forward<Fn>(fn), std::forward<Args>(args)...);
}

// ---------- Pretty printing helpers ----------
template <typename T>
struct is_streamable {
private:
    template <typename U>
    static auto test(int) -> decltype(std::declval<std::ostream&>() << std::declval<const U&>(), std::true_type{});
    template <typename>
    static auto test(...) -> std::false_type;
public:
    static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T>
std::string to_string_any(const T& v) {
    if constexpr (is_streamable<T>::value) {
        try {
            std::ostringstream oss;
            oss << v;
            return oss ? oss.str() : "<formatting failed>";
        } catch (const AssertionFailure&) { throw; }
        catch (const std::exception&) { return "<formatting threw>"; }
        catch (...) { return "<formatting threw>"; }
    } else {
        return "<value>";
    }
}

// ---------- Result aggregation ----------
template <typename T>
class ThreadSafeVector {
public:
    // 类型别名，兼容 std::vector 习惯
    using value_type = T;
    using size_type = typename std::vector<T>::size_type;
    using reference = T&;
    using const_reference = const T&;
    using iterator = typename std::vector<T>::iterator;  // 仅内部使用
    using const_iterator = typename std::vector<T>::const_iterator;
    // using difference_type = typename std::vector<T>::difference_type;

    // 默认构造/析构/移动/拷贝（均线程安全）
    ThreadSafeVector() = default;
    ~ThreadSafeVector() = default;

    // 拷贝构造（加锁保证源容器只读）
    ThreadSafeVector(const ThreadSafeVector& other) {
        std::lock_guard<std::mutex> lock(other.mtx_);
        data_ = other.data_;
    }

    // 拷贝赋值（避免自赋值 + 加锁）
    ThreadSafeVector& operator=(const ThreadSafeVector& other) {
        if (this != &other) {
            // 双锁顺序：先锁地址小的，避免死锁（可选优化）
            std::lock(mtx_, other.mtx_);
            std::lock_guard<std::mutex> lock_this(mtx_, std::adopt_lock);
            std::lock_guard<std::mutex> lock_other(other.mtx_, std::adopt_lock);
            data_ = other.data_;
        }
        return *this;
    }

    // 移动构造/赋值（无需锁，移动后原容器为空）
    ThreadSafeVector(ThreadSafeVector&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mtx_);
        data_ = std::move(other.data_);
    }

    ThreadSafeVector& operator=(ThreadSafeVector&& other) noexcept {
        if (this != &other) {
            std::lock(mtx_, other.mtx_);
            std::lock_guard<std::mutex> lock_this(mtx_, std::adopt_lock);
            std::lock_guard<std::mutex> lock_other(other.mtx_, std::adopt_lock);
            data_ = std::move(other.data_);
        }
        return *this;
    }

    std::vector<T> to_vector() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_;
    }
    [[deprecated("Use to_vector() or for_each(); iterators require external synchronization")]]
    iterator begin() {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.begin();
    }

    [[deprecated("Use to_vector() or for_each(); iterators require external synchronization")]]
    iterator end() {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.end();
    }

    [[deprecated("Use to_vector() or for_each(); iterators require external synchronization")]]
    const_iterator begin() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.begin();
    }
    [[deprecated("Use to_vector() or for_each(); iterators require external synchronization")]]
    const_iterator end() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.end();
    }

    // ========== 核心操作 ==========
    // 1. 添加元素（push_back/emplace_back）
    void push_back(const T& value) {
        std::lock_guard<std::mutex> lock(mtx_);
        data_.push_back(value);
    }

    void push_back(T&& value) {
        std::lock_guard<std::mutex> lock(mtx_);
        data_.push_back(std::move(value));
    }

    template <typename... Args>
    void emplace_back(Args&&... args) {
        std::lock_guard<std::mutex> lock(mtx_);
        data_.emplace_back(std::forward<Args>(args)...);
    }

    // 2. 访问元素（at/operator[]，at 带越界检查）
    T at(size_type index) const {
        std::lock_guard<std::mutex> lock(mtx_);
        if (index >= data_.size()) {
            throw std::out_of_range("ThreadSafeVector: index out of range");
        }
        return data_[index];  // 返回拷贝，避免外部持有引用
    }

    // 注意：operator[] 不返回引用（否则线程不安全），返回拷贝
    T operator[](size_type index) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_[index];
    }

    // 3. 获取首/尾元素（返回拷贝）
    T front() const {
        std::lock_guard<std::mutex> lock(mtx_);
        if (data_.empty()) {
            throw std::runtime_error("ThreadSafeVector: front() on empty vector");
        }
        return data_.front();
    }

    T back() const {
        std::lock_guard<std::mutex> lock(mtx_);
        if (data_.empty()) {
            throw std::runtime_error("ThreadSafeVector: back() on empty vector");
        }
        return data_.back();
    }

    // 4. 删除元素（pop_back/erase/clear）
    void pop_back() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!data_.empty()) {
            data_.pop_back();
        }
    }

    // 删除指定索引的元素（返回是否删除成功）
    bool erase(size_type index) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (index >= data_.size()) {
            return false;
        }
        data_.erase(data_.begin() + index);
        return true;
    }

    void clear(bool release_memory = false) {
        std::lock_guard<std::mutex> lock(mtx_);
        data_.clear();
        if (release_memory) std::vector<T>().swap(data_);
    }

    // 5. 容量操作
    size_type size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.empty();
    }

    void reserve(size_type new_cap) {
        std::lock_guard<std::mutex> lock(mtx_);
        data_.reserve(new_cap);
    }

    size_type capacity() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.capacity();
    }

    // 6. 批量遍历（核心！避免频繁加锁，回调内操作元素）
    // 回调函数签名：void(const T&) 或 void(T&)
    template <typename Func>
    void for_each(Func&& func) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::for_each(data_.begin(), data_.end(), std::forward<Func>(func));
    }

    // 7. 查找元素（返回第一个匹配的索引，无则返回 size()）
    template <typename Pred>
    size_type find_if(Pred&& pred) const {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = std::find_if(data_.begin(), data_.end(), std::forward<Pred>(pred));
        return it != data_.end() ? (it - data_.begin()) : data_.size();
    }

    // 8. 替换指定索引的元素（返回是否成功）
    bool replace(size_type index, const T& value) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (index >= data_.size()) {
            return false;
        }
        data_[index] = value;
        return true;
    }

    bool replace(size_type index, T&& value) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (index >= data_.size()) {
            return false;
        }
        data_[index] = std::move(value);
        return true;
    }

private:
    mutable std::mutex mtx_;          // mutable 允许 const 成员函数加锁
    std::vector<T> data_;             // 底层容器
};

struct Aggregates {
    std::atomic<count_type> cases = 0;
    std::atomic<count_type> skipped = 0;
    std::atomic<count_type> failed_cases = 0;
    std::atomic<count_type> retried_failures = 0;
    std::atomic<count_type> subcases = 0;
    std::atomic<count_type> checks = 0;
    std::atomic<count_type> failures = 0;
    std::atomic<count_type> timeouts = 0; // number of subcase or case timeouts observed
    std::atomic<count_type> retries = 0;  // number of retry attempts performed
    std::atomic<count_type> slow_cases = 0; // number of cases detected as slow
    ThreadSafeVector<std::pair<std::string,double>> slow_case_info; // name + duration ms
    ThreadSafeVector<std::pair<std::string, double>> case_times;
    ThreadSafeVector<std::pair<std::string, double>> subcase_times;
};



inline Aggregates& agg() { static Aggregates A; return A; }


template<typename Signature>
class MockFunction;


template<typename Ret, typename... Args>
class MockFunction<Ret(Args...)> {
    struct Implementation {
        std::function<Ret(Args...)> fn;
        std::recursive_mutex mutex;
        explicit Implementation(std::function<Ret(Args...)> f) : fn(std::move(f)) {}
    };
    std::shared_ptr<Implementation> impl;
    mutable std::mutex mtx;
    std::size_t call_count = 0;
    using Call = std::tuple<std::decay_t<Args>...>;
    static constexpr bool can_record = (std::is_copy_constructible_v<std::decay_t<Args>> && ...);
    bool record_calls = can_record;
    std::size_t history_limit = std::numeric_limits<std::size_t>::max();
    std::vector<Call> calls;
public:
    MockFunction() = default;
    explicit MockFunction(std::function<Ret(Args...)> f) { setImpl(std::move(f)); }
    void setImpl(std::function<Ret(Args...)> f) {
        auto next = f ? std::make_shared<Implementation>(std::move(f)) : nullptr;
        std::lock_guard<std::mutex> lock(mtx);
        impl.swap(next);
    }
    Ret operator()(Args... args) {
        std::shared_ptr<Implementation> current;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if constexpr (can_record) {
                if (record_calls && calls.size() < history_limit) calls.emplace_back(args...);
            }
            ++call_count;
            current = impl;
        }
        if (current) {
            std::lock_guard<std::recursive_mutex> lock(current->mutex);
            return current->fn(std::forward<Args>(args)...);
        }
        if constexpr (!std::is_void_v<Ret>) {
            if constexpr (std::is_default_constructible_v<Ret>) return Ret{};
            else throw std::bad_function_call();
        }
    }
    std::size_t timesCalled() const { std::lock_guard<std::mutex> lock(mtx); return call_count; }
    std::vector<Call> getCalls() const { std::lock_guard<std::mutex> lock(mtx); return calls; }
    template <typename... Values> bool calledWith(const Values&... values) const {
        const auto expected = std::tie(values...);
        std::lock_guard<std::mutex> lock(mtx);
        return std::any_of(calls.begin(), calls.end(), [&](const Call& call) { return call == expected; });
    }
    void reserveCalls(std::size_t count) {
        std::lock_guard<std::mutex> lock(mtx);
        if (record_calls) calls.reserve(std::min(count, history_limit));
    }
    void setHistoryLimit(std::size_t count) {
        std::lock_guard<std::mutex> lock(mtx);
        history_limit = count;
        if (calls.size() > count) calls.erase(calls.begin() + count, calls.end());
        if (calls.capacity() > count) calls.shrink_to_fit();
    }
    void setRecordCalls(bool enabled) {
        if (enabled && !can_record) throw std::logic_error("Mock arguments are not copyable");
        std::lock_guard<std::mutex> lock(mtx);
        record_calls = enabled;
        if (!enabled) std::vector<Call>().swap(calls);
    }
    void reset(bool release_memory = false) {
        std::lock_guard<std::mutex> lock(mtx);
        call_count = 0;
        calls.clear();
        if (release_memory) std::vector<Call>().swap(calls);
    }
};
#define CHECK_CALLED(M) CHECK((M).timesCalled() > 0)
#define CHECK_CALLED_TIMES(M,N) CHECK_EQ((M).timesCalled(), (N))
#define CHECK_CALLED_WITH(M, ...) CHECK((M).calledWith(__VA_ARGS__))

// ---------- Assertion recording ----------
inline void record_check(bool ok, const char* file, int line, std::string_view expr, std::string_view note, bool fatal, bool quiet) {
    agg().checks.fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
        // If a per-case fail counter is installed in TLS, increment it.
        if (::chtest::tls_case_fail_count) {
            ::chtest::tls_case_fail_count->fetch_add(1, std::memory_order_relaxed);
        }
        agg().failures.fetch_add(1, std::memory_order_relaxed);
        // atomic colored "FAIL" line
        ts_cout() << (Color::instance().enabled ? Color::instance().red_code() : "")
                 << "    FAIL: " << (Color::instance().enabled ? Color::instance().reset_code() : "")
                 << expr << "\n";
        // atomic dimmed location line
        ts_cout() << (Color::instance().enabled ? Color::instance().dim_code() : "")
                 << "          at " << file << ":" << line << "\n"
                 << (Color::instance().enabled ? Color::instance().reset_code() : "");
        if (!note.empty()) {
            ts_cout() << (Color::instance().enabled ? Color::instance().yellow_code() : "")
                     << "          note: " << (Color::instance().enabled ? Color::instance().reset_code() : "")
                     << note << "\n";
        }
        if (fatal) throw AssertionFailure{};
    } else {
        if (!quiet) {
            ts_cout() << (Color::instance().enabled ? Color::instance().green_code() : "")
                     << "    OK  : " << (Color::instance().enabled ? Color::instance().reset_code() : "")
                     << expr << "\n";
        }
    }
}


inline bool& current_quiet() { static bool quiet = false; return quiet; }
template <typename F> bool invoke_guarded(F&& fn, const char* description) {
    try { std::forward<F>(fn)(); return true; }
    catch (const AssertionFailure&) { return false; }
    catch (const std::exception& e) { record_check(false, __FILE__, __LINE__, description, e.what(), false, current_quiet()); }
    catch (...) { record_check(false, __FILE__, __LINE__, description, "non-std exception", false, current_quiet()); }
    return false;
}
enum class FailureMode { Check, Require, ThreadRequire };
template <typename Describe>
inline void record_assertion(bool ok, const char* file, int line, const char* expression,
                             FailureMode mode, Describe&& describe, std::string_view note = {}) {
    const bool thread_abort = mode == FailureMode::ThreadRequire && tls_case_abort;
    if (!ok && thread_abort) tls_case_abort->store(true, std::memory_order_relaxed);
    const bool fatal = mode != FailureMode::Check && !thread_abort;
    if (ok && current_quiet()) record_check(true, file, line, {}, {}, false, true);
    else if (ok) record_check(true, file, line, expression, {}, false, false);
    else record_check(false, file, line, expression, note.empty() ? std::forward<Describe>(describe)() : std::string(note), fatal, current_quiet());
}
template <typename A, typename B> std::string binary_note(const A& a, const B& b) {
    return "lhs=" + to_string_any(a) + " rhs=" + to_string_any(b);
}
template <typename A, typename B, typename Compare>
inline bool compare_values(const A& a, const B& b, Compare compare) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        if constexpr (std::is_signed_v<A> && std::is_signed_v<B>) {
            return compare(static_cast<std::intmax_t>(a), static_cast<std::intmax_t>(b));
        } else {
            if constexpr (std::is_signed_v<A>) {
                if (a < 0) return compare(-1, 0);
            } else if constexpr (std::is_signed_v<B>) {
                if (b < 0) return compare(0, -1);
            }
            return compare(static_cast<std::uintmax_t>(a), static_cast<std::uintmax_t>(b));
        }
    } else return compare(a, b);
}
#define CH_TEST_BOOL(MODE, EXPR) do { if (::chtest::assertions_enabled()) { \
    const bool ch_test_ok = static_cast<bool>(EXPR); \
    ::chtest::record_assertion(ch_test_ok, __FILE__, __LINE__, #EXPR, ::chtest::FailureMode::MODE, [] { return std::string{}; }); \
} } while (false)
#define CHECK(EXPR) CH_TEST_BOOL(Check, EXPR)
#define REQUIRE(EXPR) CH_TEST_BOOL(Require, EXPR)
#define THREAD_REQUIRE(EXPR) CH_TEST_BOOL(ThreadRequire, EXPR)
#define CH_TEST_BINARY_MODE(MODE, OP, L, R) do { if (::chtest::assertions_enabled()) { \
    [&](const auto& ch_test_l, const auto& ch_test_r) { \
    const bool ch_test_ok = ::chtest::compare_values(ch_test_l, ch_test_r, [](const auto& ch_test_a, const auto& ch_test_b) { return ch_test_a OP ch_test_b; }); \
    ::chtest::record_assertion(ch_test_ok, __FILE__, __LINE__, #L " " #OP " " #R, ::chtest::FailureMode::MODE, \
        [&] { return ::chtest::binary_note(ch_test_l, ch_test_r); }); \
    }((L), (R)); \
} } while (false)
#define CHECK_EQ(L,R) CH_TEST_BINARY_MODE(Check, ==, L, R)
#define CHECK_NE(L,R) CH_TEST_BINARY_MODE(Check, !=, L, R)
#define CHECK_LT(L,R) CH_TEST_BINARY_MODE(Check, <, L, R)
#define CHECK_LE(L,R) CH_TEST_BINARY_MODE(Check, <=, L, R)
#define CHECK_GT(L,R) CH_TEST_BINARY_MODE(Check, >, L, R)
#define CHECK_GE(L,R) CH_TEST_BINARY_MODE(Check, >=, L, R)
#define REQUIRE_EQ(L,R) CH_TEST_BINARY_MODE(Require, ==, L, R)
#define REQUIRE_NE(L,R) CH_TEST_BINARY_MODE(Require, !=, L, R)
#define REQUIRE_LT(L,R) CH_TEST_BINARY_MODE(Require, <, L, R)
#define REQUIRE_LE(L,R) CH_TEST_BINARY_MODE(Require, <=, L, R)
#define REQUIRE_GT(L,R) CH_TEST_BINARY_MODE(Require, >, L, R)
#define REQUIRE_GE(L,R) CH_TEST_BINARY_MODE(Require, >=, L, R)
#define THREAD_REQUIRE_EQ(L,R) CH_TEST_BINARY_MODE(ThreadRequire, ==, L, R)
#define THREAD_REQUIRE_NE(L,R) CH_TEST_BINARY_MODE(ThreadRequire, !=, L, R)
#define THREAD_REQUIRE_LT(L,R) CH_TEST_BINARY_MODE(ThreadRequire, <, L, R)
#define THREAD_REQUIRE_LE(L,R) CH_TEST_BINARY_MODE(ThreadRequire, <=, L, R)
#define THREAD_REQUIRE_GT(L,R) CH_TEST_BINARY_MODE(ThreadRequire, >, L, R)
#define THREAD_REQUIRE_GE(L,R) CH_TEST_BINARY_MODE(ThreadRequire, >=, L, R)

inline bool absolute_difference_within(long double left, long double right, long double tolerance) {
    if ((left < 0) != (right < 0)) {
        const auto large = std::max(std::fabs(left), std::fabs(right));
        const auto small = std::min(std::fabs(left), std::fabs(right));
        // Avoid overflow and keep the result independent of operand order.
        return large <= tolerance && small <= tolerance - large;
    }
    return std::fabs(left - right) <= tolerance;
}
template <typename A, typename B>
inline bool approx_near_abs(const A& a, const B& b, long double tolerance) {
    if (!(tolerance >= 0) || !std::isfinite(tolerance)) return false;
    const long double left = static_cast<long double>(a), right = static_cast<long double>(b);
    if (left == right) return true;
    return std::isfinite(left) && std::isfinite(right) && absolute_difference_within(left, right, tolerance);
}
template <typename A, typename B>
inline bool approx_compare_rel_abs(const A& a, const B& b, long double relative, long double absolute) {
    if (!(relative >= 0) || !(absolute >= 0) || !std::isfinite(relative) || !std::isfinite(absolute)) return false;
    const long double left = static_cast<long double>(a), right = static_cast<long double>(b);
    if (left == right) return true;
    if (!std::isfinite(left) || !std::isfinite(right)) return false;
    const long double left_magnitude = std::fabs(left), right_magnitude = std::fabs(right);
    const long double magnitude = std::max(left_magnitude, right_magnitude);
    if ((left < 0) != (right < 0)) {
        // Never form an overflowing difference, even as an intermediate.
        return absolute_difference_within(left, right, absolute) ||
               left_magnitude / magnitude + right_magnitude / magnitude <= relative;
    }
    const long double difference = std::fabs(left - right);
    return difference <= absolute || difference / magnitude <= relative;
}
#define CH_TEST_NEAR_MODE(MODE, L, R, TOL) do { if (::chtest::assertions_enabled()) { \
    [&](const auto& ch_test_l, const auto& ch_test_r, long double ch_test_tol) { \
    const bool ch_test_ok = ::chtest::approx_near_abs(ch_test_l, ch_test_r, ch_test_tol); \
    ::chtest::record_assertion(ch_test_ok, __FILE__, __LINE__, "NEAR(" #L ", " #R ")", ::chtest::FailureMode::MODE, \
        [&] { return ::chtest::binary_note(ch_test_l, ch_test_r) + " tolerance=" + ::chtest::to_string_any(ch_test_tol); }); \
    }((L), (R), (TOL)); \
} } while (false)
#define CHECK_NEAR(L,R,TOL) CH_TEST_NEAR_MODE(Check, L, R, TOL)
#define REQUIRE_NEAR(L,R,TOL) CH_TEST_NEAR_MODE(Require, L, R, TOL)
#define THREAD_REQUIRE_NEAR(L,R,TOL) CH_TEST_NEAR_MODE(ThreadRequire, L, R, TOL)
#define CH_TEST_APPROX_MODE(MODE, L, R, REL, ABS) do { if (::chtest::assertions_enabled()) { \
    [&](const auto& ch_test_l, const auto& ch_test_r, long double ch_test_rel, long double ch_test_abs) { \
    const bool ch_test_ok = ::chtest::approx_compare_rel_abs(ch_test_l, ch_test_r, ch_test_rel, ch_test_abs); \
    ::chtest::record_assertion(ch_test_ok, __FILE__, __LINE__, "APPROX(" #L ", " #R ")", ::chtest::FailureMode::MODE, \
        [&] { return ::chtest::binary_note(ch_test_l, ch_test_r) + " relative=" + ::chtest::to_string_any(ch_test_rel) + " absolute=" + ::chtest::to_string_any(ch_test_abs); }); \
    }((L), (R), (REL), (ABS)); \
} } while (false)
#define CHECK_APPROX(L,R,REL,ABS) CH_TEST_APPROX_MODE(Check, L, R, REL, ABS)
#define REQUIRE_APPROX(L,R,REL,ABS) CH_TEST_APPROX_MODE(Require, L, R, REL, ABS)
#define THREAD_REQUIRE_APPROX(L,R,REL,ABS) CH_TEST_APPROX_MODE(ThreadRequire, L, R, REL, ABS)
template <typename C, typename E> inline bool contains_in(const C& c, const E& e) {
    using Element = std::decay_t<decltype(*std::begin(c))>;
    if constexpr (std::is_integral_v<Element> && std::is_integral_v<E> &&
                  std::is_signed_v<Element> != std::is_signed_v<E>) {
        return std::find_if(std::begin(c), std::end(c), [&](const auto& value) {
            return compare_values(value, e, std::equal_to<>{});
        }) != std::end(c);
    } else return std::find(std::begin(c), std::end(c), e) != std::end(c);
}
template <typename C> auto container_size_impl(const C& c, int) -> decltype(std::size(c)) { return std::size(c); }
template <typename C> auto container_size_impl(const C& c, long) { return std::distance(std::begin(c), std::end(c)); }
template <typename C> auto container_size(const C& c) { return container_size_impl(c, 0); }
template <typename A, typename B> inline bool seq_equal_note(const A& a, const B& b, std::string& note) {
    note.clear();
    auto left = std::begin(a), left_end = std::end(a);
    auto right = std::begin(b), right_end = std::end(b);
    std::size_t index = 0;
    for (; left != left_end && right != right_end; ++left, ++right, ++index) {
        if (!compare_values(*left, *right, std::equal_to<>{})) {
            note = "mismatch at index " + std::to_string(index) + " " + binary_note(*left, *right);
            return false;
        }
    }
    if (left != left_end || right != right_end) {
        note = "size mismatch at index " + std::to_string(index);
        return false;
    }
    return true;
}
#define CH_TEST_CONTAINS_MODE(MODE, C, E) do { if (::chtest::assertions_enabled()) { \
    [&](const auto& ch_test_c, const auto& ch_test_e) { \
    const bool ch_test_ok = ::chtest::contains_in(ch_test_c, ch_test_e); \
    ::chtest::record_assertion(ch_test_ok, __FILE__, __LINE__, "CONTAINS(" #C ", " #E ")", ::chtest::FailureMode::MODE, \
        [&] { return "needle=" + ::chtest::to_string_any(ch_test_e); }); \
    }((C), (E)); \
} } while (false)
#define CHECK_CONTAINS(C,E) CH_TEST_CONTAINS_MODE(Check, C, E)
#define REQUIRE_CONTAINS(C,E) CH_TEST_CONTAINS_MODE(Require, C, E)
#define THREAD_REQUIRE_CONTAINS(C,E) CH_TEST_CONTAINS_MODE(ThreadRequire, C, E)
#define CH_TEST_SIZE_MODE(MODE, C, N) do { if (::chtest::assertions_enabled()) { \
    [&](const auto& ch_test_c, const auto& ch_test_n) { const auto ch_test_size = ::chtest::container_size(ch_test_c); \
    const bool ch_test_ok = ::chtest::compare_values(ch_test_size, ch_test_n, std::equal_to<>{}); \
    ::chtest::record_assertion(ch_test_ok, __FILE__, __LINE__, "SIZE(" #C ", " #N ")", ::chtest::FailureMode::MODE, \
        [&] { return ::chtest::binary_note(ch_test_size, ch_test_n); }); \
    }((C), (N)); \
} } while (false)
#define CHECK_SIZE(C,N) CH_TEST_SIZE_MODE(Check, C, N)
#define REQUIRE_SIZE(C,N) CH_TEST_SIZE_MODE(Require, C, N)
#define THREAD_REQUIRE_SIZE(C,N) CH_TEST_SIZE_MODE(ThreadRequire, C, N)
#define CH_TEST_SEQ_MODE(MODE, A, B) do { if (::chtest::assertions_enabled()) { \
    [&](auto&& ch_test_a, auto&& ch_test_b) { std::string ch_test_note; \
    const bool ch_test_ok = ::chtest::seq_equal_note(ch_test_a, ch_test_b, ch_test_note); \
    ::chtest::record_assertion(ch_test_ok, __FILE__, __LINE__, "SEQ_EQ(" #A ", " #B ")", ::chtest::FailureMode::MODE, \
        [&] { return ch_test_note; }); \
    }((A), (B)); \
} } while (false)
#define CHECK_SEQ_EQ(A,B) CH_TEST_SEQ_MODE(Check, A, B)
#define REQUIRE_SEQ_EQ(A,B) CH_TEST_SEQ_MODE(Require, A, B)
#define THREAD_REQUIRE_SEQ_EQ(A,B) CH_TEST_SEQ_MODE(ThreadRequire, A, B)
#define CH_TEST_EXCEPTION(MODE, EXPECTED, EXPR) do { if (::chtest::assertions_enabled()) { \
    bool ch_test_threw = false; \
    try { (void)(EXPR); } catch (const ::chtest::AssertionFailure&) { throw; } catch (...) { ch_test_threw = true; } \
    ::chtest::record_assertion(ch_test_threw == (EXPECTED), __FILE__, __LINE__, #MODE " exception(" #EXPR ")", \
        ::chtest::FailureMode::MODE, [] { return std::string{}; }); \
} } while (false)
#define CHECK_THROWS(EXPR) CH_TEST_EXCEPTION(Check, true, EXPR)
#define CHECK_NOTHROW(EXPR) CH_TEST_EXCEPTION(Check, false, EXPR)
#define REQUIRE_THROWS(EXPR) CH_TEST_EXCEPTION(Require, true, EXPR)
#define REQUIRE_NOTHROW(EXPR) CH_TEST_EXCEPTION(Require, false, EXPR)
#define THREAD_REQUIRE_THROWS(EXPR) CH_TEST_EXCEPTION(ThreadRequire, true, EXPR)
#define THREAD_REQUIRE_NOTHROW(EXPR) CH_TEST_EXCEPTION(ThreadRequire, false, EXPR)

// ---------- Output helpers ----------


inline void print_case_start(const std::string& name, int rep_idx, int rep_total) {
    // compose the optional run suffix into a single string so output stays atomic
    std::string run_suffix;
    if (rep_total > 1) run_suffix = "  (run " + std::to_string(rep_idx+1) + "/" + std::to_string(rep_total) + ")";
    ts_cout() << (Color::instance().enabled ? Color::instance().blue_code() : "")
             << "case: " << (Color::instance().enabled ? Color::instance().reset_code() : "")
             << name << run_suffix << "\n";
}
inline void print_subcase_start(const std::string& name) {
    ts_cout() << (Color::instance().enabled ? Color::instance().dim_code() : "")
             << "  subcase: " << (Color::instance().enabled ? Color::instance().reset_code() : "")
             << name << "\n";
}
inline void print_summary(count_type cases, count_type subcases, count_type checks, count_type fails, double ms) {
    ts_cout() << "\n";
    ts_cout() << (Color::instance().enabled ? Color::instance().blue_code() : "")
             << "[chtest] " << (Color::instance().enabled ? Color::instance().reset_code() : "")
             << "cases=" << cases << " subcases=" << subcases
             << " checks=" << checks << " failures=" << fails
             << " time=" << std::fixed << std::setprecision(2) << ms << "ms"
             << " skipped=" << agg().skipped.load()
             << " failed_cases=" << agg().failed_cases.load()
             << " retried_failures=" << agg().retried_failures.load()
             << " timeouts=" << ::chtest::agg().timeouts.load()
             << " retries=" << ::chtest::agg().retries.load() << "\n";
    if (fails == 0) {
        ts_cout() << (Color::instance().enabled ? Color::instance().green_code() : "") << "ALL PASSED\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    } else {
        ts_cout() << (Color::instance().enabled ? Color::instance().red_code() : "") << "SOME FAILED\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    }

    if (!agg().case_times.empty()) ts_cout() << (Color::instance().enabled ? Color::instance().blue_code() : "") << "[case timings]\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    for(const auto& ct:agg().case_times.to_vector()){
        ts_cout() << "  " << ct.first << " : " << ct.second << " ms\n";
    }

    if (!agg().subcase_times.empty()) ts_cout() << (Color::instance().enabled ? Color::instance().blue_code() : "") << "[subcase timings]\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    for(const auto& sct:agg().subcase_times.to_vector()){
        ts_cout() << "  " << sct.first << " : " << sct.second << " ms\n";
    }
    if (!agg().slow_case_info.empty()) {
        ts_cout() << (Color::instance().enabled ? Color::instance().yellow_code() : "") << "[slow cases] (threshold set)\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
        auto infos = agg().slow_case_info.to_vector();
        for (auto &p : infos) ts_cout() << "  " << p.first << " : " << p.second << " ms\n";
    }
}

// ---------- Macros (IMPL + unique names) ----------

// TEST_CASE
#define TEST_CASE_IMPL(NAME, CH_TEST_FN, CH_TEST_REG) \
static void CH_TEST_FN(); \
static ::chtest::TestRegistrar CH_TEST_REG{ NAME, CH_TEST_FN, false }; \
static void CH_TEST_FN()

#define TEST_CASE(NAME) \
TEST_CASE_IMPL(NAME, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG))

// TEST_CASE with tags: usage TEST_CASE_TAG("name", {"fast","io"})
#define TEST_CASE_TAG_IMPL(NAME, CH_TEST_FN, CH_TEST_REG, ...) \
static void CH_TEST_FN(); \
static ::chtest::TestRegistrar CH_TEST_REG{ NAME, CH_TEST_FN, false, std::vector<std::string> __VA_ARGS__ }; \
static void CH_TEST_FN()

#define TEST_CASE_TAG(NAME, ...) \
TEST_CASE_TAG_IMPL(NAME, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG), __VA_ARGS__)

// TEST_F
#define TEST_F_IMPL(FIXTURE, NAME, CH_TEST_CLASS, CH_TEST_REG) \
struct CH_TEST_CLASS : public FIXTURE { \
    static void Run() { CH_TEST_CLASS inst; inst.setUp(); try { inst.body(); } catch(...) { \
        ::chtest::invoke_guarded([&] { inst.tearDown(); }, "exception in fixture tearDown"); throw; \
    } inst.tearDown(); } \
    void body(); \
}; \
static ::chtest::TestRegistrar CH_TEST_REG{ NAME, &CH_TEST_CLASS::Run, true }; \
void CH_TEST_CLASS::body()

#define TEST_F(FIXTURE, NAME) \
TEST_F_IMPL(FIXTURE, NAME, CH_TEST_UNIQUE_NAME(CH_TEST_CLASS), CH_TEST_UNIQUE_NAME(CH_TEST_REG))


// Source location distinguishes sibling subcases with the same display name.
#define CH_TEST_SUBCASE_IMPL(NAME, ID) \
    if (::chtest::ScopedSubcaseFlag ID{(NAME), __FILE__, __LINE__}; ID.active)
#define SUBCASE(NAME) CH_TEST_SUBCASE_IMPL(NAME, CH_TEST_UNIQUE_NAME(ch_test_subcase_))

// One immutable snapshot is shared by all parameter cases: O(N) storage.
#define TEST_CASE_PARAM_IMPL(NAME, PARAMS, FN, REG) \
template <typename T> static void FN(const T& param); \
static bool REG = [] { \
    const auto values = std::make_shared<const std::decay_t<decltype(PARAMS)>>(PARAMS); \
    using Elem = typename std::decay_t<decltype(PARAMS)>::value_type; \
    for (std::size_t i = 0; i < values->size(); ++i) { \
        ::chtest::registry().push_back({ \
            std::string(NAME) + " [param " + std::to_string(i) + "]", \
            [values, i] { FN<Elem>((*values)[i]); }, {}, false, {}, 0, 0, nullptr \
        }); \
    } \
    return true; \
}(); \
template <typename T> static void FN(const T& param)
#define TEST_CASE_PARAM(NAME, PARAMS) \
TEST_CASE_PARAM_IMPL(NAME, PARAMS, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG))


// ---------- CLI parsing ----------
inline const char* help_text() {
    return "Options:\n"
        "  --test <pattern>      case-insensitive case name substring\n"
        "  --list / --cases       list filtered cases without running hooks\n"
        "  --repeat N             repeat the suite (N >= 1)\n"
        "  --shuffle <seed>       shuffle within priority groups\n"
        "  --threads N            at most N concurrent cases (N >= 1)\n"
        "  --retries N            retry failed cases up to N times\n"
        "  --timeout <ms>         cooperative deadline for each case attempt\n"
        "  --slow-threshold <ms>  report slow cases\n"
        "  --quiet                suppress successful checks and case banners\n"
        "  --no-color             disable ANSI colors\n"
        "  --no-buffer            emit output immediately\n"
        "  --buffer-limit <bytes> maximum buffered bytes per case (default 65536)\n"
        "  --timings              collect and print individual timings\n"
        "  --tag / --tag-any <tags...>  require any listed tag\n"
        "  --tag-all <tags...>     require all listed tags\n"
        "  --not-tag <tags...>     exclude any listed tag\n"
        "  --no-tag               select only untagged tests\n"
        "  --help / -h            show help\n";
}
inline Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        auto next = [&]() -> std::string_view {
            if (i + 1 >= argc || std::string_view(argv[i + 1]).substr(0, 2) == "--")
                throw std::invalid_argument("missing value for " + std::string(option));
            return argv[++i];
        };
        auto number = [&](auto& destination, bool positive = false) {
            auto value = next();
            using Value = std::decay_t<decltype(destination)>;
            Value parsed{};
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                parsed < 0 || (positive && parsed == 0))
                throw std::invalid_argument("invalid value for " + std::string(option) + ": " + std::string(value));
            destination = parsed;
        };
        auto tags = [&](std::vector<std::string>& destination, Config::TagMode mode) {
            const auto before = destination.size();
            while (i + 1 < argc && argv[i + 1][0] != '-') destination.emplace_back(argv[++i]);
            if (before == destination.size()) throw std::invalid_argument("missing tags for " + std::string(option));
            cfg.tag_mode = mode;
            cfg.tags.assign(destination.begin(), destination.end());
        };
        if (option == "--help" || option == "-h") cfg.help = true;
        else if (option == "--list") cfg.list_all = true;
        else if (option == "--cases") cfg.list_cases = true;
        else if (option == "--test") cfg.pattern = next();
        else if (option == "--repeat") number(cfg.repeat, true);
        else if (option == "--threads") number(cfg.threads, true);
        else if (option == "--retries") number(cfg.default_retries);
        else if (option == "--timeout") number(cfg.timeout_ms);
        else if (option == "--slow-threshold") number(cfg.slow_ms);
        else if (option == "--buffer-limit") number(cfg.buffer_limit, true);
        else if (option == "--shuffle") { number(cfg.seed); cfg.shuffle = true; }
        else if (option == "--quiet") cfg.quiet = true;
        else if (option == "--no-color") cfg.no_color = true;
        else if (option == "--no-buffer") cfg.no_buffer = true;
        else if (option == "--timings") cfg.timings = true;
        else if (option == "--tag" || option == "--tag-any") tags(cfg.any_tags, Config::TagMode::Any);
        else if (option == "--tag-all") tags(cfg.all_tags, Config::TagMode::All);
        else if (option == "--not-tag") tags(cfg.excluded_tags, Config::TagMode::NotAny);
        else if (option == "--no-tag") { cfg.untagged_only = true; cfg.tag_mode = Config::TagMode::NoTag; }
        else throw std::invalid_argument("unknown option: " + std::string(option));
    }
    return cfg;
}

inline void reset_aggregates() {
    auto& a = agg();
    a.cases = 0; a.subcases = 0; a.checks = 0; a.failures = 0;
    a.timeouts = 0; a.retries = 0; a.slow_cases = 0;
    a.skipped = 0; a.failed_cases = 0; a.retried_failures = 0;
    a.case_times.clear(true); a.subcase_times.clear(true); a.slow_case_info.clear(true);
}

// The test stays on its worker (including environment hooks). A watchdog sets
// the shared abort flag; it never detaches or destroys a running test's state.
class AttemptDeadline {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread watchdog;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    int timeout;
    std::atomic<bool>& abort;
    std::atomic<bool> expired{false};
public:
    AttemptDeadline(int milliseconds, std::atomic<bool>& flag) : timeout(milliseconds), abort(flag) {
        if (timeout > 0) watchdog = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cv.wait_until(lock, start + std::chrono::milliseconds(timeout), [this] { return done; })) {
                expired.store(true, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
            }
        });
    }
    bool finish() {
        if (timeout <= 0) return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!done && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(timeout)) {
                expired.store(true, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
            }
            done = true;
        }
        cv.notify_one();
        if (watchdog.joinable()) watchdog.join();
        return expired.load(std::memory_order_relaxed);
    }
    ~AttemptDeadline() { finish(); }
};
inline double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
inline void execute_case(TestCase& test, const Config& cfg, int repeat_index) {
    ContextRestore restore;
    // Aliasing shared_ptrs retain the same thread-safe lifetimes with one
    // allocation for the case's signals and output storage.
    struct CaseStorage {
        PerCaseBuffer output;
        std::atomic<bool> abort{false};
        std::atomic<count_type> failures{0};
    };
    auto storage = std::make_shared<CaseStorage>();
    std::optional<case_output_collector> collector;
    if (!cfg.no_buffer) collector.emplace(std::shared_ptr<PerCaseBuffer>(storage, &storage->output), cfg.buffer_limit);
    auto abort = std::shared_ptr<std::atomic<bool>>(storage, &storage->abort);
    auto failures = std::shared_ptr<std::atomic<count_type>>(storage, &storage->failures);
    tls_case_abort_keep = abort; tls_case_abort = abort.get();
    tls_case_fail_count_keep = failures; tls_case_fail_count = failures.get();
    route() = RouteState{};
    if (test.skip_if) {
        bool skip = false;
        if (!invoke_guarded([&] { skip = test.skip_if(); }, "exception in skip predicate")) {
            agg().cases.fetch_add(1, std::memory_order_relaxed);
            agg().failed_cases.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (skip) {
            agg().skipped.fetch_add(1, std::memory_order_relaxed);
            if (!cfg.quiet) ts_cout() << "skipping case at runtime: " << test.name << "\n";
            return;
        }
    }
    const bool measure_case = cfg.timings || cfg.slow_ms > 0;
    const auto start = measure_case ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!cfg.quiet) print_case_start(test.name, repeat_index, cfg.repeat);
    const int retries = test.retries > 0 ? test.retries : cfg.default_retries;
    for (int attempt = 0; ; ++attempt) {
        abort->store(false, std::memory_order_relaxed);
        failures->store(0, std::memory_order_relaxed);
        test.subcases.clear();
        SubcaseIndex subcase_index;
        std::optional<AttemptDeadline> deadline;
        if (cfg.timeout_ms > 0) deadline.emplace(cfg.timeout_ms, *abort);
        route() = RouteState{};
        route().mode = SubcaseMode::Discovery;
        route().current_case = &test;
        route().discovery_index = &subcase_index;
        auto* env = global_env();
        const bool setup_ok = !env || invoke_guarded([&] { env->setUp(); }, "exception in environment setUp");
        if (setup_ok) {
            // The outer guard guarantees environment cleanup even if routing or
            // timing allocation fails. Each user execution has its own guard.
            invoke_guarded([&] {
                const bool discovered = invoke_guarded(test.fn, "uncaught exception in case discovery");
                if (discovered && !abort->load(std::memory_order_relaxed)) {
                    for (std::size_t i = 0; i < test.subcases.size(); ++i) {
                        if (abort->load(std::memory_order_relaxed)) break;
                        // Nested discovery can reallocate the vector; keep this path stable.
                        const auto subcase = test.subcases[i];
                        if (!cfg.quiet) print_subcase_start(subcase.name);
                        agg().subcases.fetch_add(1, std::memory_order_relaxed);
                        route().mode = SubcaseMode::Active;
                        route().active_path = &subcase.path;
                        route().active_name = subcase.name.c_str();
                        route().path.clear();
                        route().entered_depth = 0;
                        route().in_subcase = false;
                        const auto subcase_start = cfg.timings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        invoke_guarded(test.fn, "uncaught exception in subcase");
                        route().active_path = nullptr;
                        route().active_name = nullptr;
                        if (cfg.timings) agg().subcase_times.push_back({test.name + " :: " + subcase.name, elapsed_ms(subcase_start)});
                    }
                }
            }, "exception while executing case");
            route() = RouteState{};
            route().mode = SubcaseMode::Discovery; // hook assertions are recorded once per attempt
            if (env) invoke_guarded([&] { env->tearDown(); }, "exception in environment tearDown");
        }
        route() = RouteState{};
        if (deadline && deadline->finish()) {
            record_check(false, __FILE__, __LINE__, "case timeout: " + test.name,
                         "exceeded " + std::to_string(cfg.timeout_ms) + "ms (cooperative deadline)", false, cfg.quiet);
            agg().timeouts.fetch_add(1, std::memory_order_relaxed);
        }
        const auto count = failures->load(std::memory_order_relaxed);
        if (count == 0 || attempt >= retries) break;
        agg().failures.fetch_sub(count, std::memory_order_relaxed);
        agg().retried_failures.fetch_add(count, std::memory_order_relaxed);
        agg().retries.fetch_add(1, std::memory_order_relaxed);
        if (!cfg.quiet) ts_cout() << "  retrying case: " << test.name << " (retry " << attempt + 1 << ")\n";
    }
    if (failures->load(std::memory_order_relaxed) > 0) agg().failed_cases.fetch_add(1, std::memory_order_relaxed);
    const double duration = measure_case ? elapsed_ms(start) : 0;
    if (cfg.slow_ms > 0 && duration > cfg.slow_ms) {
        agg().slow_cases.fetch_add(1, std::memory_order_relaxed);
        ts_cout() << "  SLOW: case '" << test.name << "' took " << duration << " ms\n";
        if (cfg.timings) agg().slow_case_info.push_back({test.name, duration});
    }
    if (cfg.timings) agg().case_times.push_back({test.name, duration});
    // Discovered paths belong to this invocation, not the permanent registry.
    std::vector<Subcase>().swap(test.subcases);
    agg().cases.fetch_add(1, std::memory_order_relaxed);
}

// ---------- Runner ----------
inline int run(int argc, char** argv) {
    // Global registry/configuration permit one run at a time; never deadlock a
    // nested invocation from a test or an output sink.
    static std::mutex run_mutex;
    std::unique_lock<std::mutex> running(run_mutex, std::try_to_lock);
    if (!running.owns_lock()) return 2;
    reset_aggregates();
    output_failed().store(false, std::memory_order_relaxed);
    Config cfg;
    try { cfg = parse_args(argc, argv); }
    catch (const std::exception& e) { ts_cout() << "[chtest] " << e.what() << "\nUse --help for options.\n"; return 2; }
    if (cfg.help) { ts_cout() << help_text(); return output_failed() ? 1 : 0; }
    Color::instance().set_enabled(!cfg.no_color);
    current_quiet() = cfg.quiet;
    auto& tests = registry();
    std::vector<std::size_t> indices;
    indices.reserve(tests.size());
    const auto equal_folded = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    for (std::size_t i = 0; i < tests.size(); ++i) {
        const auto& test = tests[i];
        if (!cfg.pattern.empty() && std::search(test.name.begin(), test.name.end(), cfg.pattern.begin(), cfg.pattern.end(), equal_folded) == test.name.end()) continue;
        const auto has_tag = [&](const std::string& tag) { return std::find(test.tags.begin(), test.tags.end(), tag) != test.tags.end(); };
        if (cfg.untagged_only && !test.tags.empty()) continue;
        if (!cfg.any_tags.empty() && std::none_of(cfg.any_tags.begin(), cfg.any_tags.end(), has_tag)) continue;
        if (!std::all_of(cfg.all_tags.begin(), cfg.all_tags.end(), has_tag)) continue;
        if (std::any_of(cfg.excluded_tags.begin(), cfg.excluded_tags.end(), has_tag)) continue;
        indices.push_back(i);
    }
    if (cfg.shuffle) { std::mt19937 rng(cfg.seed); std::shuffle(indices.begin(), indices.end(), rng); }
    std::stable_sort(indices.begin(), indices.end(), [&](auto a, auto b) { return tests[a].priority > tests[b].priority; });
    if (cfg.list_all || cfg.list_cases) {
        ts_cout() << "[cases]\n";
        for (const auto index : indices) ts_cout() << "  " << tests[index].name << "\n";
        return output_failed() ? 1 : 0;
    }
    const auto start = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < cfg.repeat; ++repeat) {
        auto worker = [&](std::size_t index) {
            invoke_guarded([&] { execute_case(tests[index], cfg, repeat); }, "internal case execution error");
        };
        const auto count = std::min(indices.size(), static_cast<std::size_t>(cfg.threads));
        if (count <= 1) { for (const auto index : indices) worker(index); }
        else {
            std::atomic<std::size_t> next{0};
            auto consume = [&] {
                for (;;) {
                    const auto position = next.fetch_add(1, std::memory_order_relaxed);
                    if (position >= indices.size()) break;
                    worker(indices[position]);
                }
            };
            std::vector<std::thread> workers;
            // Joining on partial thread-creation failure avoids std::terminate.
            struct Join { std::vector<std::thread>& threads; ~Join() { for (auto& thread : threads) if (thread.joinable()) thread.join(); } } join{workers};
            try {
                workers.reserve(count - 1);
                for (std::size_t i = 1; i < count; ++i) workers.emplace_back(consume);
            } catch (const std::exception& e) {
                record_check(false, __FILE__, __LINE__, "could not start all workers", e.what(), false, cfg.quiet);
            }
            consume(); // the calling thread is one of the bounded workers
        }
    }
    print_summary(agg().cases, agg().subcases, agg().checks, agg().failures, elapsed_ms(start));
    if (cfg.no_buffer) {
        try {
            std::cout.flush();
            if (!std::cout) output_failed().store(true, std::memory_order_relaxed);
        } catch (...) { output_failed().store(true, std::memory_order_relaxed); }
    }
    return agg().failures == 0 && !output_failed() ? 0 : 1;
}

} // namespace chtest

// ---------- Optional main ----------
#ifdef CH_TEST_MAIN
int main(int argc, char** argv) {
    return ::chtest::run(argc, argv);
}
#endif

#endif // CH_TEST_HPP
