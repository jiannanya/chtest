# chtest

A lightweight and  performance header only C++17 testing framework .

It is intentionally small and hackable: everything lives in one file (`include/chtest.hpp`), and the `test/` directory doubles as both a test suite and living documentation.

Key features:

- `TEST_CASE` and two-phase `SUBCASE` (discovery + active replay)
- Assertions: `CHECK`/`REQUIRE`, binary comparisons, exceptions, float helpers (`NEAR`/`APPROX`), container helpers (`CONTAINS`/`SIZE`/`SEQ_EQ`)
- Parameterized tests via `TEST_CASE_PARAM`
- Tags and CLI filtering (`--tag`, `--tag-all`, `--not-tag`, `--no-tag`, `--test <pattern>`)
- Fixtures (`TEST_F`) and a global environment hook (`chtest::Environment` + `CHTEST_SET_ENV`)
- Case-level concurrency (`--threads`) and child-thread context helpers (`with_current_case_context`, `spawn_with_context`, `THREAD_REQUIRE*`)
- Retries and scheduling (`--retries`, `TEST_CASE_RETRY`, `TEST_CASE_PRIORITY`)
- Per-case output buffering + optional output sink (`set_output_sink`)
- A simple thread-safe function mock (`chtest::MockFunction`)

Project structure:

- Header: `include/chtest.hpp`
- Example tests: `test/*.cpp`
- CMake targets: `chtest` (INTERFACE library), `chtest_tests` (example test executable)

> Note: This is a learning/teaching-style implementation. The goal is to be small, usable, and easy to read.

## Quick start

### 1) Build and run the example tests with CMake

From the repository root:

```bash
cmake -S . -B build -G Ninja
cmake --build build

# Cross-platform: run the produced executable
./build/chtest_tests
```

If you use a multi-config generator on Windows (e.g. Visual Studio), the executable may be located at:

```bash
./build/Debug/chtest_tests.exe
```

### 2) Use in your own project (as a single-header library)

The simplest integration: put `include/chtest.hpp` on your include path, then:

```cpp
#include "chtest.hpp"

TEST_CASE("math") {
    CHECK_EQ(1 + 1, 2);
}

int main(int argc, char** argv) {
    return chtest::run(argc, argv);
}
```

Alternatively, define `CH_TEST_MAIN` in a `.cpp` to let the framework provide `main`:

```cpp
#define CH_TEST_MAIN
#include "chtest.hpp"

TEST_CASE("smoke") {
    CHECK(true);
}
```

## Core concepts

### TEST_CASE / SUBCASE (single-pass discovery + replay)

`chtest` implements SUBCASE as a two-phase design: **discovery** first, then **active replay**.

- **Discovery phase**: executes the test function once, but does not execute any `SUBCASE(...)` bodies; it records which subcases exist and runs “baseline assertions” written outside SUBCASE blocks.
- **Replay phase**: executes the test function once per subcase name, and only enters the matching `SUBCASE(...)` body.

This is also why assertion macros check the current mode:

- In discovery: assertions are recorded (baseline assertions + subcase collection).
- In active replay: assertions are recorded only inside a `SUBCASE` body (`in_subcase == true`).

See `test/vector_tests.cpp` for examples.

### Fixture (TEST_F)

`TEST_F(FixtureType, "name")`:

1. Creates a fixture instance
2. Calls `setUp()`
3. Executes the test body
4. Calls `tearDown()` (guaranteed even if the body throws)

See `test/fixture_env_tests.cpp`.

### Global environment (Environment API)

You can define a global environment object (derived from `chtest::Environment`) and inject it in `main`:

```cpp
struct ResetEnv : chtest::Environment {
    void setUp() override { /* before each case */ }
    void tearDown() override { /* after each case */ }
};

ResetEnv g_env;

int main(int argc, char** argv) {
    CHTEST_SET_ENV(g_env);
    return chtest::run(argc, argv);
}
```

See `test/main.cpp` and `test/test_env.hpp`.

## Assertions

### Basic

- `CHECK(expr)` / `REQUIRE(expr)`
- Comparisons: `CHECK_EQ/NE/LT/LE/GT/GE`, `REQUIRE_*`
- Exceptions: `CHECK_THROWS(expr)`, `CHECK_NOTHROW(expr)` (and REQUIRE variants)

### Advanced

- Floating point:
  - Absolute tolerance: `CHECK_NEAR(a,b,tol)`
  - Relative + absolute tolerance: `CHECK_APPROX(a,b,rel,abs)`
- Containers:
  - `CHECK_CONTAINS(container, elem)`
  - `CHECK_SIZE(container, n)`
  - `CHECK_SEQ_EQ(a, b)` (sequence equality; on failure it provides a mismatch note)

## Parameterized tests (TEST_CASE_PARAM)

`TEST_CASE_PARAM("name", params)` registers each element in `params` as a separate case (name suffix `[param i]`).

- The test body can directly use the parameter variable `param`.
- `params` must be copyable and support `.size()` and `operator[]`.

See `test/param_and_basic_tests.cpp`.

## Tags (filter by tags)

Use `TEST_CASE_TAG("name", {"fast","math"})` to tag a test.

Filter via CLI:

- `--tag` / `--tag-any <tag...>`: keep tests that have **any** of the given tags
- `--tag-all <tag...>`: keep tests that have **all** of the given tags
- `--not-tag <tag...>`: exclude tests that have **any** of the given tags
- `--no-tag`: run only tests with no tags

See `test/tag_tests.cpp`.

## Concurrency and assertions in child threads

### Case-level concurrency

`--threads N` runs different cases concurrently via `std::async` (cases run in parallel; per-case logic is unchanged).

### Child-thread assertions and output

The framework uses thread-local state for “current case output buffer” and “routing state”. If you call `CHECK/REQUIRE` directly from a child thread, it may not be associated with the current case as you expect.

Recommended approaches:

1. Wrap your thread function with `with_current_case_context(...)` so the child thread inherits the current test context:

```cpp
std::thread t(::chtest::with_current_case_context([] {
    CHECK_EQ(1, 1);
}));
```

1. Use `spawn_with_context(...)` (equivalent semantics, shorter call site):

```cpp
std::thread t = ::chtest::spawn_with_context([] {
    CHECK(true);
});
```

See `test/threading_tests.cpp`.

### THREAD_REQUIRE: cooperative “fatal failures” in child threads

`THREAD_REQUIRE_*` is a thread-oriented “fatal” variant:

- If a per-case abort flag is installed, it does not `throw`; it sets the abort flag to true.
- If no abort flag is installed (e.g. main test thread), it behaves like `REQUIRE_*` (throws on failure).

Use together with:

- `auto abort = ::chtest::make_case_abort();`
- `spawn_with_context(abort, ...)`

so the main thread / other threads can observe the abort.

## MockFunction (simple function mock)

`chtest::MockFunction<Signature>` is a lightweight mock:

- `setImpl(std::function<...>)` to set the implementation
- `timesCalled()`, `getCalls()` to inspect usage
- Assertion helpers: `CHECK_CALLED`, `CHECK_CALLED_TIMES`, `CHECK_CALLED_WITH`

See `test/mock_tests.cpp`.

## Command line (CLI)

You can pass arguments to `chtest_tests`:

- `--test <pattern>`: filter by case name substring (case-insensitive)
- `--list`: list cases/subcases (note: subcase list currently relies on discovery collection; it may be empty without prior discovery)
- `--cases`: list only cases
- `--subcases`: list only subcases (same note as above)
- `--repeat N`: run all tests N times
- `--shuffle <seed>`: shuffle execution order with seed
- `--quiet`: suppress per-check OK lines
- `--no-color`: disable ANSI colors
- `--timeout <ms>`: run each subcase via `std::async` and enforce a timeout
- `--threads N`: case-level concurrency
- `--retries N`: global retry count (re-run failing cases up to N times)
- `--slow-threshold <ms>`: mark cases slower than the threshold

You can also use `--help` to see built-in help.

## chtest.hpp API reference

This section is the consolidated API reference for `include/chtest.hpp`.

> Version note: this document matches the `chtest.hpp` in the current repo state (2026-01-03).

### Header and namespace

- Header: `#include "chtest.hpp"`
- Namespace: `chtest`

This is a single-header implementation: registry, runner, assertion macros, subcase routing, and a simple mock all live in the same header.

### Getting started (minimal example)

```cpp
#include "chtest.hpp"

TEST_CASE("smoke") {
    CHECK_EQ(2 + 2, 4);
}

int main(int argc, char** argv) {
    return chtest::run(argc, argv);
}
```

Or:

```cpp
#define CH_TEST_MAIN
#include "chtest.hpp"

TEST_CASE("smoke") { CHECK(true); }
```

### Registration and test macros

#### TEST_CASE

```cpp
TEST_CASE("name") {
    CHECK(true);
}
```

- Registers a test case into the global registry.
- `NAME` should typically be unique and readable.

#### SUBCASE

```cpp
TEST_CASE("vector") {
    std::vector<int> v;

    CHECK(v.empty()); // baseline assertion (runs once during discovery)

    SUBCASE("push") {
        v.push_back(1);
        CHECK_EQ(v.size(), 1u);
    }

    SUBCASE("throw") {
        CHECK_THROWS(v.at(0));
    }
}
```

##### Key semantics: discovery + replay

- The framework first runs the case once in discovery mode:
  - `SUBCASE(...)` bodies are not executed
  - subcase names are collected into the current TestCase's `subcases` list
  - assertions outside SUBCASE blocks are executed once
- Then it enters active replay mode and runs the case once per subcase name:
  - only the matching `SUBCASE(...)` is entered
  - assertions are recorded only inside a `SUBCASE` body (to avoid double-counting)

#### TEST_F (fixture)

```cpp
struct MyFixture {
    void setUp() { /* ... */ }
    void tearDown() { /* ... */ }

    int value = 0;
};

TEST_F(MyFixture, "fixture sample") {
    CHECK_EQ(value, 0);
}
```

- `TEST_F(FIXTURE, NAME)` generates a derived class and calls in `Run()`:
  `inst.setUp()` → `inst.body()` → `inst.tearDown()`.
- `tearDown()` is executed even if the body throws (guarded via try/catch).

#### Parameterized: TEST_CASE_PARAM

```cpp
static auto params = std::vector<std::tuple<int,int,int>>{
    {1,2,3}, {5,7,12}
};

TEST_CASE_PARAM("add", params) {
    auto [a,b,expected] = param;
    CHECK_EQ(a + b, expected);
}
```

Implementation notes:

- The macro registers each element in `params` as an independent case (name suffix `[param i]`).
- The test body can directly use `param` (type deduced from `params::value_type`).
- `params` must be copyable and support `.size()` and `operator[]`.

#### Tags: TEST_CASE_TAG

```cpp
TEST_CASE_TAG("tagged", {"fast","math"}) {
    CHECK(true);
}
```

- The second argument is an initializer list used to construct a `std::vector<std::string>`.
- You can filter by tags using `--tag/--tag-all/--not-tag/--no-tag`.

#### Advanced registration: priority / retry / skip

- `TEST_CASE_PRIORITY("name", prio)`: higher `prio` runs first
- `TEST_CASE_RETRY("name", n)`: retry the case up to `n` times on failure
- `TEST_CASE_SKIP_IF("name", pred)`: skip at runtime if `pred` is true
- `TEST_CASE_WITH_OPTS("name", prio, retries, skip_pred)`: set all of the above in one macro

### Assertion macros

#### Basic assertions

- Boolean: `CHECK(expr)` / `REQUIRE(expr)`
- Binary comparisons:
  - `CHECK_EQ/NE/LT/LE/GT/GE(lhs, rhs)`
  - `REQUIRE_EQ/NE/...`

Semantics:

- `CHECK*`: record failure and continue
- `REQUIRE*`: throws on failure (aborts the current execution path)

#### Thread-fatal assertions: THREAD_REQUIRE*

- `THREAD_REQUIRE(expr)` / `THREAD_REQUIRE_EQ(...)` etc.

If a per-case abort flag is installed in the current thread (see `make_case_abort` below):

- failures do not throw; they set the abort flag to true (useful for cooperative stopping)

Otherwise:

- behaves like `REQUIRE*`

#### Floating point and containers

- `CHECK_NEAR(a,b,abs_tol)`: absolute tolerance
- `CHECK_APPROX(a,b,rel_tol,abs_tol)`: relative + absolute tolerance
- `CHECK_CONTAINS(container, elem)`
- `CHECK_SIZE(container, n)`
- `CHECK_SEQ_EQ(a,b)`: sequence comparison with a mismatch note

Matching `REQUIRE_*` and `THREAD_REQUIRE_*` variants also exist (see the header).

#### Exception assertions

- `CHECK_THROWS(expr)` / `REQUIRE_THROWS(expr)`
- `CHECK_NOTHROW(expr)` / `REQUIRE_NOTHROW(expr)`

### Runner and CLI

Entry point:

```cpp
int chtest::run(int argc, char** argv);
```

#### Common options

- `--test <pattern>`: filter by case name substring (case-insensitive)
- `--repeat N`
- `--shuffle <seed>`
- `--quiet`
- `--no-color`
- `--threads N`: case-level concurrency (different cases run concurrently)
- `--timeout <ms>`: enforce timeout per subcase via `std::async`
- `--retries N`: global default retry count
- `--slow-threshold <ms>`: slow-case marking

#### Listing

- `--cases`: list cases
- `--subcases`: list subcases
- `--list`: list both

Note: in the current implementation, subcases are collected during discovery into `TestCase.subcases`. If you only do listing without discovery, the subcase list may be empty.

### Output and thread-safety

#### ts_cout / per-case buffer

- `chtest::ts_cout()` returns a temporary output proxy; on destruction it flushes its internal buffer atomically.
- The runner enables a per-case buffer while a case executes:
  - writes from the case (and from child threads that inherited the context) go into the same case buffer
  - at the end of the case, the buffer is flushed once to reduce interleaving

#### Output sink

```cpp
using chtest::output_sink_t = std::function<void(std::string_view)>;

chtest::set_output_sink([](std::string_view s) {
    // e.g. forward to your logging system
});

chtest::clear_output_sink();
```

If a sink is set, all flushed output strings are forwarded to the sink; otherwise output goes to `std::cout`.

### Cross-thread context propagation

#### with_current_case_context

```cpp
auto wrapped = ::chtest::with_current_case_context([&] {
    CHECK(true);
});
std::thread t(wrapped);
```

This captures the current thread's test context (subcase routing + output buffer + abort flag, etc.) and installs it in the child thread.

#### spawn_with_context

```cpp
std::thread t = ::chtest::spawn_with_context([&] {
    CHECK(true);
});
```

This is a convenience wrapper for `with_current_case_context` + `std::thread`.

#### make_case_abort

```cpp
auto abort = ::chtest::make_case_abort();
std::thread t = ::chtest::spawn_with_context(abort, [] {
    THREAD_REQUIRE(false); // does not throw; sets abort=true
});
```

- Returns `std::shared_ptr<std::atomic<bool>>`
- Together with `THREAD_REQUIRE*`, child-thread "fatal" failures become a cooperative signal

### Global environment (Environment)

- `struct chtest::Environment { virtual void setUp(); virtual void tearDown(); }`
- `CHTEST_SET_ENV(envObj)`: set the global environment pointer

The runner calls per case:

- `global_env()->setUp()`
- execute the case (including retries)
- `global_env()->tearDown()`

### MockFunction

```cpp
chtest::MockFunction<int(int,int)> add;
add.setImpl([](int a, int b){ return a + b; });

int r = add(2,3);
CHECK_EQ(r, 5);

CHECK_CALLED(add);
CHECK_CALLED_TIMES(add, 1);
CHECK_CALLED_WITH(add, 2, 3);
```

Notes:

- Thread-safe: an internal mutex protects the call count and recorded calls
- If no impl is set and the return type is non-void, it returns a default-constructed value

### Limitations and caveats

- Assertion macros depend on the runner routing state:
  - in discovery: assertions are recorded
  - in active replay: assertions are recorded only inside `SUBCASE` (`in_subcase == true`)
  This means assertions outside SUBCASE are typically counted only once (during discovery).
- The current `--subcases/--list` subcase listing relies on discovery collection; listing-only runs may show an empty subcase list.
