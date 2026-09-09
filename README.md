# chtest

A small, single-header C++17 testing framework. The implementation lives in
`include/chtest.hpp`; examples and regression tests live in `test/`.

## Build and verify

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
./build/release/chtest_tests --quiet --no-color
```

On Windows, use `build/release/chtest_tests.exe`. Visual Studio generators place
executables under the selected configuration directory; use `--config Release`
for the build and `-C Release` for CTest.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `CHTEST_BUILD_TESTS` | `ON` | Examples, independent runner regressions, expected-failure tests |
| `CHTEST_ENABLE_ASAN` | `OFF` | AddressSanitizer on project executables |
| `CMAKE_CXX_STANDARD` | `17` | Use `20` to verify C++20 consumers |

## Integration

Copy `include/chtest.hpp` to your include path:

```cpp
#define CH_TEST_MAIN  // define in exactly one translation unit
#include "chtest.hpp"

TEST_CASE("addition") {
    CHECK_EQ(1 + 1, 2);
}
```

Alternatively, provide your own `main` and return `chtest::run(argc, argv)`.
When compiling directly, enable C++17 and link the platform thread library
(for example, `g++ -std=c++17 -pthread example.cpp`).

CMake consumers can use `add_subdirectory` and link `chtest::chtest`. Installed
packages also support `find_package(chtest CONFIG REQUIRED)`:

```sh
cmake --install build/release --prefix path/to/prefix
```

```cmake
find_package(chtest CONFIG REQUIRED)
target_link_libraries(my_tests PRIVATE chtest::chtest)
```

## Test cases and subcases

```cpp
TEST_CASE("vector") {
    std::vector<int> values;
    CHECK(values.empty());

    SUBCASE("push") {
        values.push_back(42);
        CHECK_SIZE(values, 1);
        SUBCASE("read") { CHECK_EQ(values.front(), 42); }
        SUBCASE("clear") { values.clear(); CHECK(values.empty()); }
    }
    SUBCASE("bounds") { CHECK_THROWS(values.at(0)); }
}
```

Subcases use discovery followed by replay:

1. Discovery executes the function, counts baseline assertions, and collects
   top-level subcases without entering their bodies.
2. Each collected path gets a fresh execution of the function. Only the matching
   path is entered. Nested subcases are discovered as their parents execute and
   receive additional replays.
3. Assertions are counted only at the current target depth. Baseline and parent
   assertions are suppressed on deeper replays, including their operand evaluation.

Ordinary setup statements still execute on every replay. Do not put setup side
effects exclusively inside assertions. A failed `REQUIRE` in discovery prevents
replay of partially initialized state. A failed leaf does not prevent independent
sibling leaves from running.

Name, file and line identify a subcase within its parent. Siblings with the same
name on different lines are distinct. Repeated visits to the same source location
and name are deduplicated; loop-generated branches should use distinct names.
Declare subcases on the case's worker, not in child threads. Repeated visits to
the same target path in a replay enter its body only once.

## Assertions

| Family | Forms |
| --- | --- |
| Boolean | `CHECK(expr)`, `REQUIRE(expr)` |
| Comparison | `CHECK_EQ/NE/LT/LE/GT/GE(lhs, rhs)` and `REQUIRE_*` |
| Absolute tolerance | `CHECK_NEAR(lhs, rhs, tolerance)` |
| Relative / absolute tolerance | `CHECK_APPROX(lhs, rhs, relative, absolute)` |
| Contains | `CHECK_CONTAINS(container, value)` |
| Size | `CHECK_SIZE(container, expected)` |
| Sequence equality | `CHECK_SEQ_EQ(lhs, rhs)` |
| Exceptions | `CHECK_THROWS(expr)`, `CHECK_NOTHROW(expr)` |

All families provide `REQUIRE_*` and `THREAD_REQUIRE_*` forms. `CHECK` continues
after failure. `REQUIRE` throws a dedicated framework exception to end the current
execution path; the failure is counted once. Exception assertions do not swallow
this framework control exception.

Operands are evaluated once when the assertion is active, and macros work inside
unbraced `if/else`. Operand evaluation order is not specified; avoid dependencies
between operands' side effects. Temporary containers stay alive through comparison
and diagnostic formatting. Signed and unsigned integers compare by mathematical
value (`-1` is less than `0u`, rather than wrapping).

Failures include the expression, source location and value diagnostics.
Sequence failures include the first differing index. `CHECK_SIZE` supports arrays,
sized containers and unsized ranges.

Tolerances must be finite and nonnegative. Equal infinities pass, opposite
infinities and NaNs fail. Approximate comparison uses absolute **or** relative
tolerance and handles overflow when comparing opposite large finite values.

## Parameterized tests

```cpp
static const std::vector<std::tuple<int, int, int>> params{
    {1, 2, 3}, {5, 7, 12}
};
TEST_CASE_PARAM("addition", params) {
    const auto [left, right, expected] = param;
    CHECK_EQ(left + right, expected);
}
```

Each element registers a separate case named `addition [param i]`. The container
must be copyable and provide `value_type`, `.size()` and `operator[]`. Tests use an
immutable snapshot and do not depend on the original container's later mutations
or lifetime.

## Fixtures and environments

```cpp
struct CounterFixture {
    int value = 0;
    void setUp() { value = 7; }
    void tearDown() {}
};
TEST_F(CounterFixture, "fixture") { CHECK_EQ(value, 7); }
```

A fixture is constructed and set up for each discovery/replay invocation. After
successful setup, teardown runs even when the body throws. If both the body and
teardown throw, both errors are preserved. Setup failure prevents the body and
teardown from running.

```cpp
struct ResetEnvironment : chtest::Environment {
    void setUp() override { /* before each attempt */ }
    void tearDown() override { /* after each attempt */ }
};
ResetEnvironment environment;
int main(int argc, char** argv) {
    CHTEST_SET_ENV(environment);
    return chtest::run(argc, argv);
}
```

Environment setup and teardown bracket **each attempt**, including retries.
Exceptions become failures without escaping the runner. Hooks execute on the same
worker as their test. With `--threads N`, shared environments must synchronize
shared state or use thread-local per-worker state. Configure the environment and
finish test registration before starting a run.

## Concurrency, cancellation and retries

`--threads N` uses at most N case workers, including the calling thread. Workers
consume a shared queue. A new repetition begins only after all cases from the
previous repetition finish. Priority affects dequeue order; concurrent cases can
finish in any order. Tests must not depend on another case completing first.

```cpp
TEST_CASE("child thread") {
    auto worker = chtest::spawn_with_context([] {
        CHECK_EQ(2 * 3, 6);
        REQUIRE(true);
    });
    worker.join();
}
```

`spawn_with_context` propagates routing, output, failure counters and cancellation.
It accepts move-only callables and arguments, follows `std::thread` argument
semantics, and records uncaught child exceptions. Use `std::ref` for reference
arguments. The lower-level `with_current_case_context(fn)` also supports return
values and restores the previous context on exceptions, but **rethrows** them;
catch exceptions yourself when using it directly as a `std::thread` entry point.

Join all child threads before their test invocation returns. Context propagation
does not extend the lifetime of test locals or permit deferred assertions after
the case finishes. Assertions in raw child threads without propagated context are
not routed to a test; use the helpers for reliable reporting and retries.

`THREAD_REQUIRE*` sets the installed abort flag on failure and continues. Check
`chtest::tls_case_abort->load()` in long-running loops to stop cooperatively.
Without an installed flag it throws like `REQUIRE`. You can share a custom flag:

```cpp
auto abort = chtest::make_case_abort();
auto worker = chtest::spawn_with_context(abort, [] { THREAD_REQUIRE(true); });
worker.join();
```

`--timeout ms` is a cooperative deadline for each complete attempt, including
environment hooks and subcase replays. One watchdog per active timed attempt sets
the runner's abort flag. A timeout is counted once, then the runner waits for the
test and cleanup to return. C++ cannot safely kill arbitrary in-process code:
uncooperative or deadlocked code can exceed the deadline. Watchdogs and user child
threads are additional to the `--threads` case-worker limit. A custom abort flag
is independent of the runner's deadline flag.

```cpp
TEST_CASE_RETRY("flaky", 2) { /* up to three attempts */ }
TEST_CASE_PRIORITY("important", 10) { CHECK(true); }
TEST_CASE_SKIP_IF("optional", false) { CHECK(true); }
TEST_CASE_WITH_OPTS("configured", 5, 1, ([] { return false; })) { CHECK(true); }
```

Positive per-case retries override `--retries`; zero uses the global fallback.
`TEST_CASE_PRIO` is an alias of `TEST_CASE_PRIORITY`. Skip predicates are evaluated
for every repetition, after filtering, and are not evaluated by listing commands.
Throwing predicates fail the case.

Successful recovery makes the case pass. `failures` counts failures from final
attempts; `retried_failures` counts failures discarded for retries; `checks` includes
all attempts. Earlier failure diagnostics remain visible. Every `run()` resets
counters and timing storage. Concurrent/nested `run()` calls return usage status 2.

## MockFunction

```cpp
chtest::MockFunction<int(int, int)> add;
add.setImpl([](int a, int b) { return a + b; });
CHECK_EQ(add(2, 3), 5);
CHECK_CALLED(add);
CHECK_CALLED_TIMES(add, 1);
CHECK_CALLED_WITH(add, 2, 3);
```

History stores owned, decayed argument values, including for reference parameters.
`CHECK_CALLED_WITH` searches any recorded call.
`getCalls()` returns a snapshot; `timesCalled()` returns the total invocation count.

Calls, implementation replacement and history operations are synchronized. Each
installed implementation serializes invocation, so mutable lambdas are protected;
the implementation may inspect its own history or recursively call the mock.
Replacement does not invalidate an implementation already executing. If concurrent
old/new implementations share external state, that state needs its own protection.

| Method | Behavior |
| --- | --- |
| `reserveCalls(n)` | Reserve up to the history limit when history recording is enabled |
| `setHistoryLimit(n)` | Keep the first N calls while counting all invocations |
| `setRecordCalls(false)` | Disable history and release stored argument memory |
| `reset()` | Clear history and count, retaining capacity and implementation |
| `reset(true)` | Also release history capacity |

Move-only arguments use count-only mode automatically; enabling their history
throws `std::logic_error`. Unconfigured void mocks do nothing. Other unconfigured
mocks return a default value when possible, or throw `std::bad_function_call`
(for example, reference return types).

## Output and command line

```cpp
chtest::set_output_sink([](std::string_view text) { /* consume synchronously */ });
// ... run tests ...
chtest::clear_output_sink();
```

`ts_cout()` builds one message and appends it atomically. Output is buffered per
case, with a default 64 KiB limit. Large cases flush in bounded chunks; chunks from
concurrent large cases can interleave. Individual messages can exceed the limit
and are emitted directly. Sink callbacks are serialized, can be replaced safely,
and must consume/copy the view before returning. Sink exceptions make the run fail
without throwing from output destructors. Reentrant sink logging falls back to
stdout. `--no-buffer` bypasses case buffers; it does not change process-wide C I/O
buffering. User writes directly to `std::cout` bypass framework buffering.

Messages support standard stream formatting and preserve embedded null bytes.

| Option | Meaning |
| --- | --- |
| `--test pattern` | Case-insensitive name substring |
| `--list`, `--cases` | List filtered names without running test code/hooks |
| `--tag`, `--tag-any tags...` | Require any tag in this group |
| `--tag-all tags...` | Require all tags in this group |
| `--not-tag tags...` | Exclude any tag in this group |
| `--no-tag` | Select only untagged cases |
| `--repeat N` | Run N repetitions, N >= 1 |
| `--shuffle seed` | Reproducible shuffle within priority groups |
| `--threads N` | At most N case workers, N >= 1 |
| `--retries N` | Global fallback retry count, N >= 0 |
| `--timeout ms` | Deadline per attempt; zero disables |
| `--slow-threshold ms` | Report slow cases; zero disables |
| `--quiet` | Suppress successful checks and case banners |
| `--no-color` | Disable ANSI colors |
| `--no-buffer` | Bypass case output buffering |
| `--buffer-limit bytes` | Positive per-case buffer size |
| `--timings` | Collect and print case/subcase timing details |
| `--help`, `-h` | Print help and return to the caller |

Tag groups compose with AND; repeated options append within the same group.
For example, `--tag fast --tag-all math --not-tag network` selects fast math tests
that do not have the network tag. `TEST_CASE_TAG("name", {"fast", "math"})`
supports comma-separated initializer lists directly.

Unknown options, missing arguments, overflow, trailing numeric garbage and invalid
ranges return 2. Passing runs return 0; test/output failures return 1. An empty
selection returns 0 and reports zero cases. Use `--timings` for individual case
and subcase timing details.
Aggregate and per-attempt failure counters use `chtest::count_type` (`uint64_t`),
including summaries and retry accounting.

`ThreadSafeVector` offers snapshots (`to_vector()`), locked `for_each`, copied
element access and synchronized mutation. Its legacy raw iterators are deprecated:
they require external synchronization for their entire lifetime.

## Regression tests

`chtest_tests` is the passing suite. `chtest_regressions` independently checks
runner results, counts and output, including failure paths. Deliberately failing
examples run in `chtest_failure_examples`; CTest verifies both their exit code
and exact failure summary so a runtime crash cannot count as an expected pass.

CTest also includes:

- Three fixed-seed property runs: exhaustive mixed 8-bit comparisons, exact integer
  oracles for floating comparisons, and random sequence/vector/mock state models.
- All 42 assertion variants in both success and failure scenarios, including
  fatal/continuing/cancellation behavior, broken printers and 64-bit accounting.
- Concurrent output, sink replacement, nested child failures, retries, mock
  replacement, vector access and competing runner invocations.
- Memory checks for history limits and repeated timed/untimed runs.
- Independent installed-package consumers in two translation units, using both
  standard and custom header/library installation directories.

```sh
ctest --test-dir build/release --output-on-failure -j 4
ctest --test-dir build/release -L stress --repeat until-fail:20 --output-on-failure
ctest --test-dir build/release -L memory --output-on-failure
```

On supported Linux GCC/Clang toolchains, sanitizers are explicit build options.
ASan and UBSan can be combined; TSan requires a separate build. UBSan findings
stop execution, so CTest cannot silently pass after an undefined-behavior report.

```sh
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCHTEST_ENABLE_ASAN=ON -DCHTEST_ENABLE_UBSAN=ON
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure
```
