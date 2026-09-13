"""Compare two executables built from test/benchmarks.cpp (stdlib only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess


def pin_cpu(cpu):
    """Constrain this probe driver and its children, never the caller's shell."""
    if cpu < 0:
        raise ValueError("CPU index must be nonnegative")
    if os.name == "nt":
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.GetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t))
        kernel.SetProcessAffinityMask.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
        process = kernel.GetCurrentProcess()
        allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not kernel.GetProcessAffinityMask(process, ctypes.byref(allowed), ctypes.byref(system)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not allowed.value & (1 << cpu):
            raise ValueError("CPU index is outside this process's allowed processor group")
        if not kernel.SetProcessAffinityMask(process, 1 << cpu):
            raise ctypes.WinError(ctypes.get_last_error())
    elif hasattr(os, "sched_setaffinity"):
        if cpu not in os.sched_getaffinity(0):
            raise ValueError("CPU index is outside this process's allowed CPUs")
        os.sched_setaffinity(0, {cpu})
    else:
        raise ValueError("CPU affinity is not supported on this platform")


MODES = ("assertions", "params", "scheduler", "output", "output-small-buffer", "output-formatted", "subcases")


def measure(executable, mode, repetitions=1):
    env = dict(os.environ, CHTEST_BENCHMARK_PARAMS="512" if mode == "params" else "0")
    result = subprocess.run([str(executable), mode, str(repetitions)], env=env, check=True,
                            capture_output=True, text=True, timeout=60)
    values = {}
    for field in result.stdout.strip().split():
        key, value = field.split("=", 1)
        values[key] = value if key == "mode" else float(value)
    expected_checks = {"assertions": 200000, "params": 512, "scheduler": 4096, "output": 0,
                       "output-small-buffer": 0, "output-formatted": 0, "subcases": 512}
    if values.get("mode") != mode or values.get("checks") != expected_checks[mode] * repetitions:
        raise RuntimeError(f"{executable}: unexpected workload for {mode}: {values}")
    output_bytes = {"output": 12800000, "output-small-buffer": 12800000, "output-formatted": 81978890}
    if mode in output_bytes and values.get("output_bytes", 0) < output_bytes[mode] * repetitions:
        raise RuntimeError(f"{executable}: output workload was not fully emitted")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--repetitions", type=int, default=1, help="repeat the workload inside each measured run (1-1000)")
    parser.add_argument("--cpu", type=int, help="pin probes to one allowed logical CPU, useful on hybrid CPUs")
    parser.add_argument("--modes", nargs="+", choices=MODES,
                        default=("assertions", "params", "scheduler", "output"))
    parser.add_argument("--output", type=Path, default=Path("build/benchmark-results.json"))
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    if not 1 <= args.repetitions <= 1000:
        parser.error("--repetitions must be between 1 and 1000")
    if args.cpu is not None:
        try:
            pin_cpu(args.cpu)
        except (ValueError, OSError) as error:
            parser.error(str(error))
    executables = {"before": args.before.resolve(strict=True), "after": args.after.resolve(strict=True)}
    report = {"runs": args.runs, "repetitions": args.repetitions, "cpu": args.cpu,
              "executables": {key: str(value) for key, value in executables.items()}, "results": {}}
    report["executable_sha256"] = {key: hashlib.sha256(value.read_bytes()).hexdigest() for key, value in executables.items()}
    for mode in args.modes:
        samples = {key: [] for key in executables}
        for executable in executables.values():
            measure(executable, mode, args.repetitions)  # warmup excluded from statistics
        for index in range(args.runs):
            for key in (list(executables) if index % 2 == 0 else list(reversed(executables))):
                samples[key].append(measure(executables[key], mode, args.repetitions))
        summaries = {key: {field: statistics.median(sample[field] for sample in rows)
                           for field in rows[0] if field != "mode"} for key, rows in samples.items()}
        report["results"][mode] = {"median": summaries, "samples": samples}
        before, after = summaries["before"], summaries["after"]
        print(f"{mode}: {before['elapsed_ms']:.3f} -> {after['elapsed_ms']:.3f} ms; "
              f"peak memory {before['peak_memory_mib']:.2f} -> {after['peak_memory_mib']:.2f} MiB", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
