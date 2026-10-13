# chtest performance optimization record

The runner prepares a case-folding and substring skip table once per run, avoids
sorting already ordered priorities, reuses output allocations between cases, and
reuses watchdog threads for timed attempts. Assertion counters still update
immediately; retries, priority order and cooperative cancellation keep their
existing behavior. The implementation remains a single C++17 header using
standard-library threads, clocks and synchronization, with no new dependency.

Completed cases recycle buffer capacity on their worker; the capacity is released
when the run finishes. Empty output does not allocate a payload buffer. Each timed
worker reuses a watchdog across attempts and cases. Finishing an attempt
synchronously disarms its deadline before the next attempt can start.

## Reproducing the benchmarks

Enable `CHTEST_BUILD_BENCHMARKS` in a Release build. Compile the **same version** of
`test/benchmarks.cpp` against each header being compared; older benchmark sources
do not implement all probes. For example, with Clang or GCC:

```sh
mkdir -p build/baseline-include
git show 9df7f4d:include/chtest.hpp > build/baseline-include/chtest.hpp
c++ -std=c++17 -O3 -DNDEBUG -pthread -I build/baseline-include test/benchmarks.cpp -o build/benchmark-before
c++ -std=c++17 -O3 -DNDEBUG -pthread -I include test/benchmarks.cpp -o build/benchmark-after
python3 tools/benchmark.py --before build/benchmark-before --after build/benchmark-after \
  --runs 9 --repetitions 20 \
  --modes assertions params scheduler scheduler-serial timed filter output-cases output output-small-buffer output-formatted subcases \
  --output build/performance-comparison.json
```

Windows benchmark builds link `psapi` through CMake. The Python driver uses only
the standard library, validates workload counts and output volume, alternates
before/after order, and saves raw samples, medians and executable hashes. CPU
pinning is optional and available only on platforms that support it. Filtering
happens once per process; `--repetitions` repeats selected cases after filtering.

## Results

Measured on macOS arm64 with AppleClang 17, C++17 `-O3 -DNDEBUG`, against `9df7f4d`
on 2026-10-13 (9 samples, 20 repetitions, median elapsed time):

| Workload | Before | After | Elapsed-time reduction |
| --- | ---: | ---: | ---: |
| 4,096 short cases with deadlines | 1,284.88 ms | 18.00 ms | 98.6% |
| Sparse selection among 32,769 registered probe cases | 5.08 ms | 1.16 ms | 77.2% |
| 4,096 cases with buffered output | 47.79 ms | 34.52 ms | 27.8% |
| Serial short-case scheduling | 5.45 ms | 5.05 ms | 7.2% |
| Parallel short-case scheduling | 7.44 ms | 6.63 ms | 10.8% |
| Parameterized cases | 0.701 ms | 0.676 ms | 3.5% |
| Quiet assertions | 7.16 ms | 7.18 ms | -0.3% |
| Subcase replay | 33.34 ms | 32.86 ms | 1.4% |
| Buffered output | 41.40 ms | 37.24 ms | 10.1% |
| Small-buffer output | 40.68 ms | 42.42 ms | -4.3% |
| Large numeric/text messages | 33.24 ms | 33.92 ms | -2.0% |

These are synthetic workloads on one machine, not cross-platform speed guarantees.
Sub-millisecond runs and allocation-heavy output vary with system load; use longer
runs when investigating small differences. Negative reductions indicate slower
samples; improvements are concentrated in the changed paths. Peak memory was
similar or lower (the largest increase was 0.02 MiB).

## Validation

Local C++17, C++20 and ASan/UBSan runs each passed all 16 CTest tests.
The TSan run passed 15/16, including 10 repeated runs of each concurrency, output and
performance regression test. Its allocation-counting probe reports retained bytes
during string growth on both the baseline and optimized headers under Apple TSan;
the same memory contracts pass in ordinary and ASan/UBSan builds. Linux/Windows
validation is configured in CI and was not executed on this macOS host.
