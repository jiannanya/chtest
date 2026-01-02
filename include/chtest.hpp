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
#include <future>
#include <cmath>
#include <iterator>
#include <string_view>

namespace chtest {
// Per-case buffer holding an ostringstream and a mutex. Each running case
// owns one of these (stack-allocated in `case_output_collector`) and
// child threads may append to it while it exists. The mutex serializes
// appends to the buffer; this avoids races when multiple threads write to
// the same case buffer.
struct PerCaseBuffer {
    std::ostringstream buf;
    std::mutex mtx;
};

// Per-thread pointer to the current case's buffer (or nullptr). Threads do
// not inherit TLS automatically; use the provided helpers to propagate the
// pointer into spawned threads.
inline thread_local PerCaseBuffer* tls_case_out = nullptr;
// Keep a shared_ptr to the current case buffer so it can be propagated and
// kept alive across threads when using the context helpers.
inline thread_local std::shared_ptr<PerCaseBuffer> tls_case_out_keep = nullptr;
// Keep a shared_ptr to the current case abort flag so it can be propagated
// and kept alive across threads when using the context helpers.
inline thread_local std::shared_ptr<std::atomic<bool>> tls_case_abort_keep = nullptr;
// Per-thread pointer to a shared atomic used to signal a fatal failure inside
// a case from any thread. The pointer value is provided by the case owner
// (worker) and may point into a shared_ptr kept alive for the case lifetime.
inline thread_local std::atomic<bool>* tls_case_abort = nullptr;
// Per-case failure counter keeper: child threads inherit a shared_ptr to
// this atomic so failures recorded from any thread during a case attempt
// can be counted specifically for that case.
inline thread_local std::shared_ptr<std::atomic<int>> tls_case_fail_count_keep = nullptr;
inline thread_local std::atomic<int>* tls_case_fail_count = nullptr;

// Global flush mutex used when emitting a completed case buffer to stdout.
inline std::mutex& global_out_mutex() { static std::mutex m; return m; }


// Optional external output sink.
// If set, all flushed strings will be forwarded to this sink.
// Otherwise, output defaults to `std::cout`.
//
// Thread-safety: atomic shared_ptr so writers don't race with set/clear.
using output_sink_t = std::function<void(std::string_view)>;
inline std::atomic<std::shared_ptr<output_sink_t>>& output_sink() {
    static std::atomic<std::shared_ptr<output_sink_t>> sink{nullptr};
    return sink;
}

inline void set_output_sink(output_sink_t sink) {
    output_sink().store(std::make_shared<output_sink_t>(std::move(sink)), std::memory_order_release);
}

inline void clear_output_sink() {
    output_sink().store(nullptr, std::memory_order_release);
}

inline void emit_output(std::string_view s) {
    if (s.empty()) return;
    auto sink = output_sink().load(std::memory_order_acquire);
    if (sink) {
        (*sink)(s);
        return;
    }
    std::cout << s;
}

// Thread-safe output proxy: accumulate into an ostringstream and flush
// to std::cout while holding a mutex when the temporary is destroyed.
struct ts_ostream_proxy {
    std::ostringstream ss;
    ts_ostream_proxy() = default;
    template <typename T>
    ts_ostream_proxy& operator<<(const T& v) {
        ss << v;
        return *this;
    }
    // ensure manipulators like std::endl/std::flush work
    ts_ostream_proxy& operator<<(std::ostream& (*manip)(std::ostream&)) {
        ss << manip;
        return *this;
    }
    ~ts_ostream_proxy() noexcept {
        // If there's a per-case buffer installed, lock that buffer's mutex
        // and append into it. Otherwise fall back to writing directly to
        // stdout (protected by the global_out_mutex to preserve ordering).
        if (tls_case_out) {
            std::lock_guard<std::mutex> lk(tls_case_out->mtx);
            tls_case_out->buf << ss.str();
        } else {
            std::lock_guard<std::mutex> lk(global_out_mutex());
            emit_output(ss.str());
        }
    }
};

inline ts_ostream_proxy ts_cout() { return ts_ostream_proxy(); }

struct case_output_collector {
    std::shared_ptr<PerCaseBuffer> buf;
    case_output_collector() : buf(std::make_shared<PerCaseBuffer>()) {
        tls_case_out_keep = buf;
        tls_case_out = buf.get();
    }
    ~case_output_collector() noexcept {
        // Lock the per-buffer mutex to synchronize with writers and
        // capture the current contents. Then clear the TLS pointer and
        // keeper so any subsequent writers will write directly to stdout.
        // Finally, emit the captured string under the global_out_mutex to
        // preserve ordering with other direct stdout writes.
        std::string s;
        {
            std::lock_guard<std::mutex> lk(buf->mtx);
            s = buf->buf.str();
            tls_case_out = nullptr;
            tls_case_out_keep.reset();
        }
        if (!s.empty()) {
            std::lock_guard<std::mutex> lk(global_out_mutex());
            emit_output(s);
        }
    }
    // non-copyable
    case_output_collector(const case_output_collector&) = delete;
    case_output_collector& operator=(const case_output_collector&) = delete;
};

// (moved: helper functions for propagating case context and spawn helpers
// are defined later, after RouteState is declared)


// ---------- Unique name helpers ----------
#define CH_TEST_CONCAT_INNER(a,b) a##b
#define CH_TEST_CONCAT(a,b) CH_TEST_CONCAT_INNER(a,b)
#define CH_TEST_UNIQUE_NAME(base) CH_TEST_CONCAT(base, __COUNTER__)

// ---------- Config ----------
struct Config {
    std::string pattern;
    bool list_all = false;      // --list
    bool list_cases = false;    // --cases
    bool list_subcases = false; // --subcases
    int repeat = 1;             // --repeat N
    bool shuffle = false;       // --shuffle <seed>
    unsigned seed = 0;
    bool quiet = false;         // --quiet
    bool no_color = false;      // --no-color
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
    virtual void setUp() {}
    virtual void tearDown() {}
};
inline Environment*& global_env() { static Environment* env=nullptr; return env; }

#define CHTEST_SET_ENV(ENVOBJ) ::chtest::global_env() = &(ENVOBJ);


// ---------- Test case & registry ----------
struct Subcase {
    std::string name;
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
#define TEST_CASE_PRIO(NAME, PRIO) \
TEST_CASE_IMPL(NAME, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG)) \
/* the above macro expands into a registrar; override below by reassigning priority */

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

struct RouteState {
    SubcaseMode mode = SubcaseMode::Normal;
    const char* active_name = nullptr;   // subcase currently executing
    TestCase* current_case = nullptr;    // case under discovery/execution
    bool in_subcase = false;
};

inline RouteState& route() {
    static thread_local RouteState R;
    return R;
}

struct ScopedSubcaseFlag {
    bool active=true;
    ScopedSubcaseFlag(){ route().in_subcase=true; }
    ~ScopedSubcaseFlag(){ route().in_subcase=false; }
};

// Called inside SUBCASE macro to decide execution & register
inline bool subcase_enter(const char* name) {
    auto& rt = route();
    if (rt.mode == SubcaseMode::Active) {
        return (rt.active_name && std::string(rt.active_name) == name);
    }
    if (rt.mode == SubcaseMode::Discovery) {
        if (rt.current_case) {
            rt.current_case->subcases.push_back(Subcase{ name });
        }
        return false; // do not execute subcase body during discovery/base
    }
    // Normal base run: skip subcase bodies
    return false;
}

// Helper functions to allow child threads to observe the current test/subcase
// context. Must be defined after RouteState/route() so route() is visible.
template <typename F>
inline std::function<void()> with_current_case_context(F f, std::shared_ptr<std::atomic<bool>> abort_keep = nullptr) {
    // capture current thread-local route, case output pointer and keepers
    auto route_snapshot = route();
    PerCaseBuffer* outptr = tls_case_out;
    auto out_keep = tls_case_out_keep;
    auto abort_keep_local = abort_keep ? abort_keep : tls_case_abort_keep;
    std::atomic<bool>* abort_ptr = abort_keep_local ? abort_keep_local.get() : nullptr;

    return [route_snapshot, outptr, out_keep = std::move(out_keep), abort_keep_local = std::move(abort_keep_local), abort_ptr, f = std::move(f)]() mutable {
        // save current thread's values so we can restore them
        auto prev_route = route();
        PerCaseBuffer* prev_out = tls_case_out;
        auto prev_out_keep = tls_case_out_keep;
        std::atomic<bool>* prev_abort = tls_case_abort;
        auto prev_abort_keep = tls_case_abort_keep;

        // install the captured context
        route() = route_snapshot;
        tls_case_out = outptr;
        tls_case_out_keep = out_keep;
        tls_case_abort = abort_ptr;
        tls_case_abort_keep = abort_keep_local;

        try {
            f();
        } catch(...) {
            // restore before rethrowing
            route() = prev_route;
            tls_case_out = prev_out;
            tls_case_out_keep = prev_out_keep;
            tls_case_abort = prev_abort;
            tls_case_abort_keep = prev_abort_keep;
            throw;
        }

        // restore previous context
        route() = prev_route;
        tls_case_out = prev_out;
        tls_case_out_keep = prev_out_keep;
        tls_case_abort = prev_abort;
        tls_case_abort_keep = prev_abort_keep;
    };
}

// Create a shared abort flag for a case. Keep the returned shared_ptr alive
// for the duration of the case (or until all spawned threads finish). Use
// this together with `with_current_case_context(..., abort_ptr)` or
// `spawn_with_context` so child threads see the flag.
inline std::shared_ptr<std::atomic<bool>> make_case_abort() {
    return std::make_shared<std::atomic<bool>>(false);
}

// Spawn a std::thread that inherits the current test/subcase context (route,
// tls_case_out) and optionally a shared abort flag. Returns a std::thread
// object the caller must join.
template <typename Fn, typename... Args>
inline std::thread spawn_with_context(std::shared_ptr<std::atomic<bool>> abort_keep, Fn&& fn, Args&&... args) {
    auto bound = std::bind(std::forward<Fn>(fn), std::forward<Args>(args)...);
    auto wrapped = with_current_case_context([bound]() mutable { bound(); }, std::move(abort_keep));
    return std::thread(std::move(wrapped));
}

template <typename Fn, typename... Args>
inline std::thread spawn_with_context(Fn&& fn, Args&&... args) {
    auto bound = std::bind(std::forward<Fn>(fn), std::forward<Args>(args)...);
    auto wrapped = with_current_case_context([bound]() mutable { bound(); });
    return std::thread(std::move(wrapped));
}

// ---------- Pretty printing helpers ----------
template <typename T>
struct is_streamable {
private:
    template <typename U>
    static auto test(int) -> decltype(std::declval<std::ostream&>() << std::declval<U>(), std::true_type{});
    template <typename>
    static auto test(...) -> std::false_type;
public:
    static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T>
std::string to_string_any(const T& v) {
    if constexpr (is_streamable<T>::value) {
        std::ostringstream oss;
        oss << v;
        return oss.str();
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
    iterator begin() {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.begin();
    }

    iterator end() {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.end();
    }

    const_iterator begin() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return data_.begin();
    }
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

    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        data_.clear();
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
    std::atomic<int> cases = 0;
    std::atomic<int> subcases = 0;
    std::atomic<int> checks = 0;
    std::atomic<int> failures = 0;
    std::atomic<int> timeouts = 0; // number of subcase or case timeouts observed
    std::atomic<int> retries = 0;  // number of retry attempts performed
    std::atomic<int> slow_cases = 0; // number of cases detected as slow
    ThreadSafeVector<std::pair<std::string,double>> slow_case_info; // name + duration ms
    ThreadSafeVector<std::pair<std::string, double>> case_times;
    ThreadSafeVector<std::pair<std::string, double>> subcase_times;
};



inline Aggregates& agg() { static Aggregates A; return A; }


template<typename Signature>
class MockFunction;

template<typename Ret, typename... Args>
class MockFunction<Ret(Args...)> {
    std::function<Ret(Args...)> impl;
    mutable std::mutex mtx;
    int call_count = 0;
    std::vector<std::tuple<Args...>> calls;

public:
    MockFunction() = default;
    MockFunction(std::function<Ret(Args...)> f) : impl(std::move(f)) {}

    // 设置实现
    void setImpl(std::function<Ret(Args...)> f) {
        std::lock_guard<std::mutex> lock(mtx);
        impl = std::move(f);
    }

    // 调用
    Ret operator()(Args... args) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            call_count++;
            calls.emplace_back(args...);
        }
        if (impl) return impl(args...);
        if constexpr (!std::is_void_v<Ret>) return Ret{};
    }

    // 查询调用次数
    int timesCalled() const {
        std::lock_guard<std::mutex> lock(mtx);
        return call_count;
    }

    // 获取调用参数
    std::vector<std::tuple<Args...>> getCalls() const {
        std::lock_guard<std::mutex> lock(mtx);
        return calls;
    }

    // 清理记录
    void reset() {
        std::lock_guard<std::mutex> lock(mtx);
        call_count = 0;
        calls.clear();
    }
};

// ---------- 辅助断言宏 ----------
#define CHECK_CALLED(M) CHECK((M).timesCalled() > 0)
#define CHECK_CALLED_TIMES(M,N) CHECK((M).timesCalled() == (N))
#define CHECK_CALLED_WITH(M, ...) \
    CHECK((M).getCalls().size() > 0 && (M).getCalls().back() == std::make_tuple(__VA_ARGS__))



// ---------- Assertion recording ----------
inline void record_check(bool ok, const char* file, int line, const std::string& expr, const std::string& note, bool fatal, bool quiet) {
    agg().checks++;
    if (!ok) {
        // If a per-case fail counter is installed in TLS, increment it.
        if (::chtest::tls_case_fail_count) {
            ::chtest::tls_case_fail_count->fetch_add(1, std::memory_order_relaxed);
        }
        agg().failures++;
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
        if (fatal) throw std::runtime_error("REQUIRE failed");
    } else {
        if (!quiet) {
            ts_cout() << (Color::instance().enabled ? Color::instance().green_code() : "")
                     << "    OK  : " << (Color::instance().enabled ? Color::instance().reset_code() : "")
                     << expr << "\n";
        }
    }
}

// boolean
#define CHECK(EXPR) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
        ::chtest::record_check((EXPR), __FILE__, __LINE__, #EXPR, "", false, ::chtest::current_quiet())

#define REQUIRE(EXPR) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
        ::chtest::record_check((EXPR), __FILE__, __LINE__, #EXPR, "", true, ::chtest::current_quiet())

// THREAD_REQUIRE: thread-safe variant of REQUIRE. If a per-case abort flag
// (`tls_case_abort`) is installed in the thread, THREAD_REQUIRE will record
// the check and, on failure, set the abort flag instead of throwing. If no
// abort flag is installed (i.e. running in the main test thread), it behaves
// exactly like REQUIRE (throws on failure).
#define THREAD_REQUIRE(EXPR) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    bool ok__ = (EXPR); \
    if (::chtest::tls_case_abort) { \
        ::chtest::record_check(ok__, __FILE__, __LINE__, #EXPR, "", false, ::chtest::current_quiet()); \
        if (!ok__) *::chtest::tls_case_abort = true; \
    } else { \
        ::chtest::record_check(ok__, __FILE__, __LINE__, #EXPR, "", true, ::chtest::current_quiet()); \
    } \
} while(0)


// binary
#define CH_TEST_BINARY(opname, op, fatal, L, R)\
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do {                                                      \
    auto&& lhs__ = (L);                                     \
    auto&& rhs__ = (R);                                     \
    bool ok__ = (lhs__ op rhs__);                           \
    std::ostringstream expr__;                              \
    expr__ << #opname " (" #L " " #op " " #R ")  lhs="      \
           << ::chtest::to_string_any(lhs__)             \
           << " rhs=" << ::chtest::to_string_any(rhs__); \
    ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), "", fatal, ::chtest::current_quiet()); \
} while(0)

#define CHECK_EQ(L,R) CH_TEST_BINARY(EQ,==,false,L,R)
#define CHECK_NE(L,R) CH_TEST_BINARY(NE,!=,false,L,R)
#define CHECK_LT(L,R) CH_TEST_BINARY(LT,< ,false,L,R)
#define CHECK_LE(L,R) CH_TEST_BINARY(LE,<=,false,L,R)
#define CHECK_GT(L,R) CH_TEST_BINARY(GT,> ,false,L,R)
#define CHECK_GE(L,R) CH_TEST_BINARY(GE,>=,false,L,R)

#define REQUIRE_EQ(L,R) CH_TEST_BINARY(EQ,==,true,L,R)
#define REQUIRE_NE(L,R) CH_TEST_BINARY(NE,!=,true,L,R)
#define REQUIRE_LT(L,R) CH_TEST_BINARY(LT,< ,true,L,R)
#define REQUIRE_LE(L,R) CH_TEST_BINARY(LE,<=,true,L,R)
#define REQUIRE_GT(L,R) CH_TEST_BINARY(GT,> ,true,L,R)
#define REQUIRE_GE(L,R) CH_TEST_BINARY(GE,>=,true,L,R)

// Thread-aware binary comparison: if a per-case abort flag exists, record
// failure and set the flag; otherwise behave like REQUIRE_* (throw on fail).
#define CH_TEST_THREAD_BINARY(opname, op, L, R) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do {                                                      \
    auto&& lhs__ = (L);                                     \
    auto&& rhs__ = (R);                                     \
    bool ok__ = (lhs__ op rhs__);                           \
    std::ostringstream expr__;                              \
    expr__ << #opname " (" #L " " #op " " #R ")  lhs="      \
           << ::chtest::to_string_any(lhs__)             \
           << " rhs=" << ::chtest::to_string_any(rhs__); \
    if (::chtest::tls_case_abort) { \
        ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), "", false, ::chtest::current_quiet()); \
        if (!ok__) *::chtest::tls_case_abort = true; \
    } else { \
        ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), "", true, ::chtest::current_quiet()); \
    } \
} while(0)

#define THREAD_REQUIRE_EQ(L,R) CH_TEST_THREAD_BINARY(EQ,==,L,R)
#define THREAD_REQUIRE_NE(L,R) CH_TEST_THREAD_BINARY(NE,!=,L,R)
#define THREAD_REQUIRE_LT(L,R) CH_TEST_THREAD_BINARY(LT,< ,L,R)
#define THREAD_REQUIRE_LE(L,R) CH_TEST_THREAD_BINARY(LE,<=,L,R)
#define THREAD_REQUIRE_GT(L,R) CH_TEST_THREAD_BINARY(GT,> ,L,R)
#define THREAD_REQUIRE_GE(L,R) CH_TEST_THREAD_BINARY(GE,>=,L,R)

// ---------- Advanced assertions ----------
// Floating-point approx/near comparisons (absolute tolerance)
template <typename A, typename B>
inline bool approx_near_abs(const A& a, const B& b, long double abs_tol) {
    long double la = (long double)a;
    long double lb = (long double)b;
    if (std::isfinite(la) && std::isfinite(lb)) {
        return std::fabsl(la - lb) <= abs_tol;
    }
    return la == lb;
}

#define CH_TEST_NEAR_IMPL(fatal, L, R, TOL) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    auto&& lhs__ = (L); \
    auto&& rhs__ = (R); \
    bool ok__ = ::chtest::approx_near_abs(lhs__, rhs__, (long double)(TOL)); \
    std::ostringstream expr__; \
    expr__ << "NEAR(" #L "," #R ", tol=" << (TOL) << ")  lhs=" \
           << ::chtest::to_string_any(lhs__) << " rhs=" << ::chtest::to_string_any(rhs__); \
    ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), "", fatal, ::chtest::current_quiet()); \
} while(0)

#define CHECK_NEAR(L,R,TOL) CH_TEST_NEAR_IMPL(false, L, R, TOL)
#define REQUIRE_NEAR(L,R,TOL) CH_TEST_NEAR_IMPL(true, L, R, TOL)
#define THREAD_REQUIRE_NEAR(L,R,TOL) \
    if (::chtest::tls_case_abort) { \
        CH_TEST_NEAR_IMPL(false, L, R, TOL); \
        if (!::chtest::approx_near_abs((L),(R),(long double)(TOL))) *::chtest::tls_case_abort = true; \
    } else CH_TEST_NEAR_IMPL(true, L, R, TOL)

// Approx with relative + absolute tolerance: pass if |a-b| <= max(abs_tol, rel_tol * max(|a|,|b|))
template <typename A, typename B>
inline bool approx_compare_rel_abs(const A& a, const B& b, long double rel_tol, long double abs_tol) {
    long double la = (long double)a;
    long double lb = (long double)b;
    if (std::isfinite(la) && std::isfinite(lb)) {
        long double diff = std::fabsl(la - lb);
        long double mag = std::fmaxl(std::fabsl(la), std::fabsl(lb));
        long double threshold = std::fmaxl(abs_tol, rel_tol * mag);
        return diff <= threshold;
    }
    return la == lb;
}

#define CH_TEST_APPROX_IMPL(fatal, L, R, REL, ABS) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    auto&& lhs__ = (L); \
    auto&& rhs__ = (R); \
    bool ok__ = ::chtest::approx_compare_rel_abs(lhs__, rhs__, (long double)(REL), (long double)(ABS)); \
    std::ostringstream expr__; \
    expr__ << "APPROX(" #L "," #R ", rel=" << (REL) << ", abs=" << (ABS) << ")  lhs=" \
           << ::chtest::to_string_any(lhs__) << " rhs=" << ::chtest::to_string_any(rhs__); \
    ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), "", fatal, ::chtest::current_quiet()); \
} while(0)

#define CHECK_APPROX(L,R,REL,ABS) CH_TEST_APPROX_IMPL(false, L, R, REL, ABS)
#define REQUIRE_APPROX(L,R,REL,ABS) CH_TEST_APPROX_IMPL(true, L, R, REL, ABS)
#define THREAD_REQUIRE_APPROX(L,R,REL,ABS) \
    if (::chtest::tls_case_abort) { \
        CH_TEST_APPROX_IMPL(false, L, R, REL, ABS); \
        if (!::chtest::approx_compare_rel_abs((L),(R),(long double)(REL),(long double)(ABS))) *::chtest::tls_case_abort = true; \
    } else CH_TEST_APPROX_IMPL(true, L, R, REL, ABS)

// Container helpers
template <typename Container, typename Elem>
inline bool contains_in(const Container& c, const Elem& e) {
    return std::find(std::begin(c), std::end(c), e) != std::end(c);
}

// Sequence equality with nice mismatch note
template <typename A, typename B>
inline bool seq_equal_note(const A& a, const B& b, std::string& note) {
    auto it1 = std::begin(a), end1 = std::end(a);
    auto it2 = std::begin(b), end2 = std::end(b);
    size_t idx = 0;
    for (; it1 != end1 && it2 != end2; ++it1, ++it2, ++idx) {
        if (!(*it1 == *it2)) {
            note = "mismatch at index " + std::to_string(idx) + " lhs=" + ::chtest::to_string_any(*it1) + " rhs=" + ::chtest::to_string_any(*it2);
            return false;
        }
    }
    if (it1 != end1 || it2 != end2) {
        auto s1 = static_cast<long long>(std::distance(std::begin(a), std::end(a)));
        auto s2 = static_cast<long long>(std::distance(std::begin(b), std::end(b)));
        note = "size mismatch lhs_size=" + std::to_string(s1) + " rhs_size=" + std::to_string(s2);
        return false;
    }
    return true;
}

#define CH_TEST_CONTAINS_IMPL(fatal, C, E) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    auto&& c__ = (C); \
    auto&& e__ = (E); \
    bool ok__ = ::chtest::contains_in(c__, e__); \
    std::ostringstream expr__; \
    expr__ << "CONTAINS(" #C "," #E ")  needle=" << ::chtest::to_string_any(e__) << " in=" << ::chtest::to_string_any(c__); \
    ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), "", fatal, ::chtest::current_quiet()); \
} while(0)

#define CHECK_CONTAINS(C,E) CH_TEST_CONTAINS_IMPL(false, C, E)
#define REQUIRE_CONTAINS(C,E) CH_TEST_CONTAINS_IMPL(true, C, E)
#define THREAD_REQUIRE_CONTAINS(C,E) \
    if (::chtest::tls_case_abort) { \
        CH_TEST_CONTAINS_IMPL(false, C, E); \
        if (!::chtest::contains_in((C),(E))) *::chtest::tls_case_abort = true; \
    } else CH_TEST_CONTAINS_IMPL(true, C, E)

#define CH_TEST_SIZE_IMPL(fatal, C, N) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    auto&& c__ = (C); \
    auto n = static_cast<long long>(N); \
    auto sz = static_cast<long long>(std::distance(std::begin(c__), std::end(c__))); \
    bool ok__ = (sz == n); \
    std::ostringstream expr__; \
    expr__ << "SIZE(" #C ") expected=" << n << " got=" << sz; \
    ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), "", fatal, ::chtest::current_quiet()); \
} while(0)

#define CHECK_SIZE(C,N) CH_TEST_SIZE_IMPL(false, C, N)
#define REQUIRE_SIZE(C,N) CH_TEST_SIZE_IMPL(true, C, N)
#define THREAD_REQUIRE_SIZE(C,N) \
    if (::chtest::tls_case_abort) { \
        CH_TEST_SIZE_IMPL(false, C, N); \
        long long _n = static_cast<long long>(N); \
        long long _sz = static_cast<long long>(std::distance(std::begin((C)), std::end((C)))); \
        if (_sz != _n) *::chtest::tls_case_abort = true; \
    } else CH_TEST_SIZE_IMPL(true, C, N)

#define CH_TEST_SEQ_EQ_IMPL(fatal, A, B) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    auto&& a__ = (A); \
    auto&& b__ = (B); \
    std::string note__; \
    bool ok__ = ::chtest::seq_equal_note(a__, b__, note__); \
    std::ostringstream expr__; \
    expr__ << "SEQ_EQ(" #A "," #B ")  lhs=" << ::chtest::to_string_any(a__) << " rhs=" << ::chtest::to_string_any(b__); \
    ::chtest::record_check(ok__, __FILE__, __LINE__, expr__.str(), note__, fatal, ::chtest::current_quiet()); \
} while(0)

#define CHECK_SEQ_EQ(A,B) CH_TEST_SEQ_EQ_IMPL(false, A, B)
#define REQUIRE_SEQ_EQ(A,B) CH_TEST_SEQ_EQ_IMPL(true, A, B)
#define THREAD_REQUIRE_SEQ_EQ(A,B) \
    if (::chtest::tls_case_abort) { \
        CH_TEST_SEQ_EQ_IMPL(false, A, B); \
        std::string _note; \
        if (!::chtest::seq_equal_note((A),(B), _note)) *::chtest::tls_case_abort = true; \
    } else CH_TEST_SEQ_EQ_IMPL(true, A, B)


// exception assertions
#define CH_TEST_THROWS_CORE(fatal, EXPR) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    bool threw__ = false; \
    try { (void)(EXPR); } catch(...) { threw__ = true; } \
    ::chtest::record_check(threw__, __FILE__, __LINE__, "THROWS(" #EXPR ")", "", fatal, ::chtest::current_quiet()); \
} while(0)
#define CH_TEST_NOTHROW_CORE(fatal, EXPR) \
    if (::chtest::route().mode == ::chtest::SubcaseMode::Discovery || \
        (::chtest::route().mode == ::chtest::SubcaseMode::Active && ::chtest::route().in_subcase)) \
do { \
    bool threw__ = false; \
    try { (void)(EXPR); } catch(...) { threw__ = true; } \
    ::chtest::record_check(!threw__, __FILE__, __LINE__, "NOTHROW(" #EXPR ")", "", fatal, ::chtest::current_quiet()); \
} while(0)

#define CHECK_THROWS(EXPR) CH_TEST_THROWS_CORE(false, EXPR)
#define CHECK_NOTHROW(EXPR) CH_TEST_NOTHROW_CORE(false, EXPR)
#define REQUIRE_THROWS(EXPR) CH_TEST_THROWS_CORE(true, EXPR)
#define REQUIRE_NOTHROW(EXPR) CH_TEST_NOTHROW_CORE(true, EXPR)

// ---------- Output helpers ----------
inline bool& current_quiet() { static bool q=false; return q; }

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
inline void print_summary(int cases, int subcases, int checks, int fails, double ms) {
    ts_cout() << "\n";
    ts_cout() << (Color::instance().enabled ? Color::instance().blue_code() : "")
             << "[chtest] " << (Color::instance().enabled ? Color::instance().reset_code() : "")
             << "cases=" << cases << " subcases=" << subcases
             << " checks=" << checks << " failures=" << fails
             << " time=" << std::fixed << std::setprecision(2) << ms << "ms"
             << " timeouts=" << ::chtest::agg().timeouts.load()
             << " retries=" << ::chtest::agg().retries.load() << "\n";
    if (fails == 0) {
        ts_cout() << (Color::instance().enabled ? Color::instance().green_code() : "") << "ALL PASSED\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    } else {
        ts_cout() << (Color::instance().enabled ? Color::instance().red_code() : "") << "SOME FAILED\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    }

    ts_cout() << (Color::instance().enabled ? Color::instance().blue_code() : "") << "[case timings]\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    for(auto& ct:agg().case_times){
        ts_cout() << "  " << ct.first << " : " << ct.second << " ms\n";
    }

    ts_cout() << (Color::instance().enabled ? Color::instance().blue_code() : "") << "[subcase timings]\n" << (Color::instance().enabled ? Color::instance().reset_code() : "");
    for(auto& sct:agg().subcase_times){
        ts_cout() << "  " << sct.first << " : " << sct.second << " ms\n";
    }
    if (agg().slow_cases.load() > 0) {
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
#define TEST_CASE_TAG_IMPL(NAME, CH_TEST_FN, CH_TEST_REG, TAGSVEC) \
static void CH_TEST_FN(); \
static ::chtest::TestRegistrar CH_TEST_REG{ NAME, CH_TEST_FN, false, std::vector<std::string> TAGSVEC }; \
static void CH_TEST_FN()

#define TEST_CASE_TAG(NAME, TAGSVEC) \
TEST_CASE_TAG_IMPL(NAME, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG), TAGSVEC)

// TEST_F
#define TEST_F_IMPL(FIXTURE, NAME, CH_TEST_CLASS, CH_TEST_REG) \
struct CH_TEST_CLASS : public FIXTURE { \
    static void Run() { CH_TEST_CLASS inst; inst.setUp(); try { inst.body(); } catch(...) { inst.tearDown(); throw; } inst.tearDown(); } \
    void body(); \
}; \
static ::chtest::TestRegistrar CH_TEST_REG{ NAME, &CH_TEST_CLASS::Run, true }; \
void CH_TEST_CLASS::body()

#define TEST_F(FIXTURE, NAME) \
TEST_F_IMPL(FIXTURE, NAME, CH_TEST_UNIQUE_NAME(CH_TEST_CLASS), CH_TEST_UNIQUE_NAME(CH_TEST_REG))

// SUBCASE: single-pass discovery, then active replay matching by name
// Usage inside TEST_CASE function body:
//   static constexpr const char* CH_TEST_CURRENT_CASE = "case name"; //
//   injected automatically below if (subcase_enter("branch")) { ... }
// #define SUBCASE(NAME) \
// if (::chtest::subcase_enter(NAME))

#define SUBCASE(NAME) \
for (bool once=true; once && ::chtest::subcase_enter(NAME); once=false) \
    for (::chtest::ScopedSubcaseFlag flag; flag.active; flag.active=false)

// TEST_CASE_PARAM: registers each param as its own case calling a function with `param`
// #define TEST_CASE_PARAM_IMPL(NAME, PARAMS, CH_TEST_FN, CH_TEST_REG) \
// static void CH_TEST_FN(const decltype(PARAMS)::value_type& param); \
// static bool CH_TEST_REG = [](){ \
//     auto values = PARAMS; \
//     for (size_t i=0;i<values.size();++i) { \
//         ::chtest::registry().push_back({ \
//             std::string(NAME) + " [param " + std::to_string(i) + "]", \
//             [=](){ CH_TEST_FN(values[i]); }, {}, false \
//         }); \
//     } \
//     return true; \
// }(); \
// static void CH_TEST_FN(const decltype(PARAMS)::value_type& param)
#define TEST_CASE_PARAM_IMPL(NAME, PARAMS, FN, REG) \
template <typename T> static void FN(const T& param); \
static bool REG = [](){ \
    auto values = PARAMS; \
    using Elem = typename decltype(values)::value_type; \
    for (size_t i=0;i<values.size();++i) { \
        ::chtest::registry().push_back({ \
            std::string(NAME) + " [param " + std::to_string(i) + "]", \
            [=](){ FN<Elem>(values[i]); }, {}, false \
        }); \
    } \
    return true; \
}(); \
template <typename T> static void FN(const T& param)

#define TEST_CASE_PARAM(NAME, PARAMS) \
TEST_CASE_PARAM_IMPL(NAME, PARAMS, CH_TEST_UNIQUE_NAME(CH_TEST_FN), CH_TEST_UNIQUE_NAME(CH_TEST_REG))

// ---------- CLI parsing ----------
inline Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nextS = [&](std::string& dst){ if (i+1 < argc) dst = argv[++i]; };
        auto nextI = [&](int& dst){ if (i+1 < argc) dst = std::stoi(argv[++i]); };
        auto nextU = [&](unsigned& dst){ if (i+1 < argc) dst = static_cast<unsigned>(std::stoul(argv[++i])); };

        if (a == "--list") cfg.list_all = true;
        else if (a == "--cases") cfg.list_cases = true;
        else if (a == "--subcases") cfg.list_subcases = true;
        else if (a == "--repeat") nextI(cfg.repeat);
        else if (a == "--shuffle") { cfg.shuffle = true; nextU(cfg.seed); }
        else if (a == "--quiet") cfg.quiet = true;
        else if (a == "--no-color") cfg.no_color = true;
        else if (a == "--test")nextS(cfg.pattern);
        else if (a == "--tag" || a == "--tag-any") {
            cfg.tag_mode = Config::TagMode::Any;
            // consume one or more tag arguments following --tag until the next
            // option (starting with '-') or end of argv
            while (i+1 < argc && argv[i+1][0] != '-') {
                cfg.tags.push_back(std::string(argv[++i]));
            }
        }
        else if (a == "--tag-all") {
            cfg.tag_mode = Config::TagMode::All;
            while (i+1 < argc && argv[i+1][0] != '-') {
                cfg.tags.push_back(std::string(argv[++i]));
            }
        }
        else if (a == "--no-tag") {
            cfg.tag_mode = Config::TagMode::NoTag;
        }
        else if (a == "--not-tag") {
            cfg.tag_mode = Config::TagMode::NotAny;
            while (i+1 < argc && argv[i+1][0] != '-') {
                cfg.tags.push_back(std::string(argv[++i]));
            }
        }
        else if(a=="--timeout") nextI(cfg.timeout_ms);
        else if(a=="--threads") nextI(cfg.threads);
        else if(a=="--retries") nextI(cfg.default_retries);
        else if(a=="--slow-threshold") nextI(cfg.slow_ms);
        else if (a == "--help" || a == "-h") {
            ts_cout() <<
                "Options:\n"
                "  --test <pattern>   filter case names (substring, case-insensitive)\n"
                "  --list             list all cases and subcases\n"
                "  --cases            list only case names\n"
                "  --subcases         list only subcases\n"
                "  --repeat N         repeat all tests N times\n"
                "  --shuffle <seed>   shuffle order with seed\n"
                "  --quiet            suppress per-check OK lines\n"
                "  --no-color         disable colors\n"
                "  --tag <tag> / --tag-any <tag>   filter tests to those having any of the given tags (can repeat)\n"
                "  --tag-all <tag>    require tests to have ALL listed tags\n"
                "  --not-tag <tag>    exclude tests that have ANY of the listed tags\n"
                "  --no-tag           run tests that have no tags assigned\n"
                "  --help             show this help\n"
                "  --h                same as help\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ---------- Runner ----------
inline int run(int argc, char** argv) {
    auto cfg = parse_args(argc, argv);
    Color::instance().set_enabled(!cfg.no_color);
    current_quiet() = cfg.quiet;

    // Build indices with filter
    auto& tests = registry();
    std::vector<int> indices;
    auto match = [&](const std::string& s, const std::string& p) {
        if (p.empty()) return true;
        auto lower = [](std::string v){ for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return v; };
        return lower(s).find(lower(p)) != std::string::npos;
    };
    for (int i = 0; i < (int)tests.size(); ++i) {
        if (!match(tests[i].name, cfg.pattern)) continue;
        // dynamic skip: if a runtime predicate is provided and returns true, skip this case
        if (tests[i].skip_if) {
            bool do_skip = false;
            try { do_skip = tests[i].skip_if(); } catch(...) { do_skip = false; }
            if (do_skip) {
                if (!cfg.quiet) ts_cout() << "skipping case at runtime: " << tests[i].name << "\n";
                continue;
            }
        }
        // Tag filtering modes
        if (cfg.tag_mode == Config::TagMode::NoTag) {
            if (!tests[i].tags.empty()) continue;
        } else if (cfg.tag_mode == Config::TagMode::Any) {
            if (!cfg.tags.empty()) {
                bool has = false;
                for (auto &tf: cfg.tags) {
                    for (auto &tt: tests[i].tags) {
                        if (tf == tt) { has = true; break; }
                    }
                    if (has) break;
                }
                if (!has) continue;
            }
        } else if (cfg.tag_mode == Config::TagMode::All) {
            if (!cfg.tags.empty()) {
                bool all_ok = true;
                for (auto &tf: cfg.tags) {
                    bool found = false;
                    for (auto &tt: tests[i].tags) {
                        if (tf == tt) { found = true; break; }
                    }
                    if (!found) { all_ok = false; break; }
                }
                if (!all_ok) continue;
            }
        } else if (cfg.tag_mode == Config::TagMode::NotAny) {
            if (!cfg.tags.empty()) {
                bool any_found = false;
                for (auto &tf: cfg.tags) {
                    for (auto &tt: tests[i].tags) {
                        if (tf == tt) { any_found = true; break; }
                    }
                    if (any_found) break;
                }
                if (any_found) continue; // exclude tests that have any of the given tags
            }
        }
        indices.push_back(i);
    }

    // Shuffle if requested
    if (cfg.shuffle) {
        std::mt19937 rng(cfg.seed);
        std::shuffle(indices.begin(), indices.end(), rng);
    }

    // Priority scheduling: stable sort by priority descending so higher priority runs first
    std::stable_sort(indices.begin(), indices.end(), [&](int a, int b){
        return tests[a].priority > tests[b].priority;
    });

    // Listing
    if (cfg.list_all || cfg.list_cases || cfg.list_subcases) {
        if (cfg.list_all || cfg.list_cases) {
            Color::instance().blue(); ts_cout() << "[cases]\n"; Color::instance().reset();
            for (int idx : indices) ts_cout() << "  " << tests[idx].name << "\n";
        }
        if (cfg.list_all || cfg.list_subcases) {
            Color::instance().blue(); ts_cout() << "[subcases]\n"; Color::instance().reset();
            for (int idx : indices) {
                for (auto& sc : tests[idx].subcases) {
                    ts_cout() << "  " << tests[idx].name << " :: " << sc.name << "\n";
                }
            }
        }
        return 0;
    }

    auto t_begin = std::chrono::steady_clock::now();
    // int exit_failures = 0;

    std::mutex out_mutex;



    for (int r = 0; r < std::max(1, cfg.repeat); ++r) {

        auto worker =
            [&](int idx) {
                auto& T = tests[idx];
                // collect all output produced while running this case into
                // a per-case buffer and flush it atomically at the end.
                ::chtest::case_output_collector case_out_collector;
                // Install a per-case abort flag and keep it alive for the
                // duration of this case execution. child threads that
                // inherit context will capture this shared_ptr and thus
                // will be able to set the abort flag via THREAD_REQUIRE.
                auto case_abort = ::chtest::make_case_abort();
                // per-case fail counter to attribute failures to this case
                auto case_fail_count = std::make_shared<std::atomic<int>>(0);
                struct AbortKeeper {
                    std::shared_ptr<std::atomic<bool>> prev_abort_keep;
                    std::atomic<bool>* prev_abort_ptr;
                    std::shared_ptr<std::atomic<int>> prev_fail_keep;
                    std::atomic<int>* prev_fail_ptr;
                    AbortKeeper(std::shared_ptr<std::atomic<bool>> new_abort_keep,
                                std::shared_ptr<std::atomic<int>> new_fail_keep) {
                        prev_abort_keep = ::chtest::tls_case_abort_keep;
                        prev_abort_ptr = ::chtest::tls_case_abort;
                        prev_fail_keep = ::chtest::tls_case_fail_count_keep;
                        prev_fail_ptr = ::chtest::tls_case_fail_count;
                        ::chtest::tls_case_abort_keep = new_abort_keep;
                        ::chtest::tls_case_abort = new_abort_keep.get();
                        ::chtest::tls_case_fail_count_keep = new_fail_keep;
                        ::chtest::tls_case_fail_count = new_fail_keep.get();
                    }
                    ~AbortKeeper() {
                        ::chtest::tls_case_abort = prev_abort_ptr;
                        ::chtest::tls_case_abort_keep = prev_abort_keep;
                        ::chtest::tls_case_fail_count = prev_fail_ptr;
                        ::chtest::tls_case_fail_count_keep = prev_fail_keep;
                    }
                } abort_keeper(case_abort, case_fail_count);
                if (!cfg.quiet) print_case_start(T.name, r, cfg.repeat);
                auto case_start = std::chrono::steady_clock::now();

                if (global_env()) global_env()->setUp();

                // Determine number of attempts: 1 + retries
                int per_case_retries = (T.retries > 0) ? T.retries : cfg.default_retries;
                int max_attempts = 1 + per_case_retries;

                for (int attempt = 1; attempt <= max_attempts; ++attempt) {
                    if (!cfg.quiet) ts_cout() << "  attempt " << attempt << "/" << max_attempts << "\n";

                    // reset abort flag for this attempt
                    case_abort->store(false);

                    // clear previously discovered subcases to avoid duplication on retry
                    T.subcases.clear();

                    // capture per-case failures before attempt so we can detect new failures attributed to this case
                    int fails_before = case_fail_count->load();

                    // Single-pass discovery + base assertions
                    route().mode = SubcaseMode::Discovery;
                    route().current_case = &T;
                    route().active_name = nullptr;
                    try {
                        T.fn();  // registers subcases and runs base assertions once
                    } catch (const std::exception& e) {
                        record_check(false, __FILE__, __LINE__,
                                    "uncaught exception in case discovery", e.what(),
                                    true, cfg.quiet);
                    } catch (...) {
                        record_check(false, __FILE__, __LINE__,
                                    "uncaught non-std exception in case discovery", "",
                                    true, cfg.quiet);
                    }
                    route().mode = SubcaseMode::Normal;
                    route().current_case = nullptr;

                    // Execute each subcase path independently
                    for (auto& sc : T.subcases) {
                        if (!cfg.quiet) print_subcase_start(sc.name);
                        agg().subcases++;
                        route().mode = SubcaseMode::Active;
                        route().active_name = sc.name.c_str();
                        auto sc_start = std::chrono::steady_clock::now();

                        if (cfg.timeout_ms > 0) {
                            // Run the subcase in an async task so we can enforce a
                            // timeout. Use a per-subcase abort flag so child threads
                            // can cooperatively stop when timeout occurs.
                            auto sub_abort = ::chtest::make_case_abort();
                            auto wrapped = ::chtest::with_current_case_context([&T]() {
                                try {
                                    T.fn();
                                } catch (const std::exception& e) {
                                    ::chtest::record_check(false, __FILE__, __LINE__,
                                                              "uncaught exception in subcase",
                                                              e.what(), true, ::chtest::current_quiet());
                                } catch (...) {
                                    ::chtest::record_check(false, __FILE__, __LINE__,
                                                              "uncaught non-std exception in subcase",
                                                              "", true, ::chtest::current_quiet());
                                }
                            }, sub_abort);

                            auto fut = std::async(std::launch::async, std::move(wrapped));
                            if (fut.wait_for(std::chrono::milliseconds(cfg.timeout_ms)) == std::future_status::timeout) {
                                // signal abort to cooperative child threads
                                sub_abort->store(true);
                                ::chtest::record_check(false, __FILE__, __LINE__, "subcase timeout: [" + T.name + " :: " + sc.name + "]",
                                                          "exceeded " + std::to_string(cfg.timeout_ms) + "ms",
                                                          false, cfg.quiet);
                                ::chtest::agg().timeouts++;
                            } else {
                                // completed in time: rethrow any exception from the task
                                try {
                                    fut.get();
                                } catch (const std::exception& e) {
                                    ::chtest::record_check(false, __FILE__, __LINE__,
                                                              "uncaught exception in subcase",
                                                              e.what(), true, cfg.quiet);
                                } catch (...) {
                                    ::chtest::record_check(false, __FILE__, __LINE__,
                                                              "uncaught non-std exception in subcase",
                                                              "", true, cfg.quiet);
                                }
                            }
                        } else {
                            try {
                                T.fn();
                            } catch (const std::exception& e) {
                                record_check(false, __FILE__, __LINE__,
                                            "uncaught exception in subcase", e.what(), true,
                                            cfg.quiet);
                            } catch (...) {
                                record_check(false, __FILE__, __LINE__,
                                            "uncaught non-std exception in subcase", "", true,
                                            cfg.quiet);
                            }
                        }
                        auto sc_end = std::chrono::steady_clock::now();
                        double sc_ms =
                            std::chrono::duration<double, std::milli>(sc_end - sc_start)
                                .count();
                        agg().subcase_times.push_back({T.name + " :: " + sc.name, sc_ms});
                        route().mode = SubcaseMode::Normal;
                        route().active_name = nullptr;
                    }

                    if (global_env()) global_env()->tearDown();

                    // if this case produced any failures during this attempt, consider retrying
                    int fails_after = case_fail_count->load();
                    bool failed = (fails_after > fails_before);
                    if (failed && attempt < max_attempts) {
                        if (!cfg.quiet) ts_cout() << "  case failed, will retry: " << T.name << " (next attempt " << (attempt+1) << ")\n";
                        ::chtest::agg().retries++;
                        // continue to next attempt
                        continue;
                    }
                    // either success or no more attempts
                    break;
                }

                auto case_end=std::chrono::steady_clock::now();
                double case_ms=std::chrono::duration<double,std::milli>(case_end-case_start).count();
                if(cfg.timeout_ms>0 && case_ms>cfg.timeout_ms){
                    record_check(false, __FILE__, __LINE__, "case timeout: [" + T.name + "]",
                                "exceeded " + std::to_string(cfg.timeout_ms) + "ms",
                                false, cfg.quiet);
                    ::chtest::agg().timeouts++;
                }
                // slow-case detection: non-fatal marking and record
                if (cfg.slow_ms > 0 && case_ms > cfg.slow_ms) {
                    // print a colored SLOW notice
                    ts_cout() << (Color::instance().enabled ? Color::instance().yellow_code() : "")
                             << "  SLOW: case '" << T.name << "' took " << std::fixed << std::setprecision(2) << case_ms
                             << " ms (threshold " << cfg.slow_ms << " ms)\n"
                             << (Color::instance().enabled ? Color::instance().reset_code() : "");
                    ::chtest::agg().slow_cases++;
                    ::chtest::agg().slow_case_info.push_back({T.name, case_ms});
                }
                agg().case_times.push_back({T.name,case_ms});
                agg().cases++;
            };
        
    // 并发执行
        std::vector<std::future<void>> futures;
        for(int idx:indices){
            if(cfg.threads>1){
                futures.push_back(std::async(std::launch::async,worker,idx));
            }else{
                worker(idx);
            }
        }
        for(auto& f:futures) f.get();
    }

    auto t_end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_begin).count();
    print_summary(agg().cases, agg().subcases, agg().checks, agg().failures, ms);

    return agg().failures==0?0:1;
}

} // namespace chtest

// ---------- Optional main ----------
#ifdef CH_TEST_MAIN
int main(int argc, char** argv) {
    return ::chtest::run(argc, argv);
}
#endif

#endif // CH_TEST_HPP
