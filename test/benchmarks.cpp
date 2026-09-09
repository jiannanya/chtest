#include "chtest.hpp"
#include <array>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {
struct Parameter {
    inline static std::size_t copies = 0;
    std::array<char, 512> payload{};
    Parameter() = default;
    Parameter(const Parameter& other) : payload(other.payload) { ++copies; }
};
// Override to zero for unrelated probes, so registration does not pollute
// their peak-memory measurements. The comparison script sets this per mode.
const std::vector<Parameter> parameters([] {
#ifdef _WIN32
    char text[32]{};
    const auto size = GetEnvironmentVariableA("CHTEST_BENCHMARK_PARAMS", text, sizeof(text));
    const char* count = size > 0 && size < sizeof(text) ? text : nullptr;
#else
    const char* count = std::getenv("CHTEST_BENCHMARK_PARAMS");
#endif
    return count ? static_cast<std::size_t>(std::strtoul(count, nullptr, 10)) : std::size_t{512};
}());
TEST_CASE_PARAM("parameter", parameters) { CHECK_EQ(param.payload[0], 0); }

TEST_CASE("quiet binary assertions") {
    for (int i = 0; i < 200000; ++i) CHECK_EQ(i, i);
}
TEST_CASE("buffered output") {
    const std::string line(127, 'x');
    for (int i = 0; i < 100000; ++i) chtest::ts_cout() << line << '\n';
}
const std::vector<std::string> subcase_names = [] {
    std::vector<std::string> names;
    for (int i = 0; i < 512; ++i) names.push_back("long subcase name used to expose repeated allocation during matching: " + std::to_string(i));
    return names;
}();
TEST_CASE("many subcases") {
    for (const auto& name : subcase_names) {
        SUBCASE(name.c_str()) { CHECK(true); }
    }
}
double peak_memory_mib() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<double>(counters.PeakWorkingSetSize) / 1048576.0;
#elif defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
        return static_cast<double>(usage.ru_maxrss) / 1048576.0;
#else
        return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
    }
#endif
    return -1;
}
}
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "assertions";
    std::atomic<int> active{0}, peak{0};
    if (mode == "scheduler") {
        for (int i = 0; i < 4096; ++i) {
            chtest::registry().push_back({"scheduled " + std::to_string(i), [&] {
                const int count = active.fetch_add(1) + 1;
                int prior = peak.load();
                while (count > prior && !peak.compare_exchange_weak(prior, count)) {}
                CHECK(true);
                active.fetch_sub(1);
            }, {}, false, {}, 0, 0, nullptr});
        }
    }
    const std::string pattern = mode == "subcases" ? "many subcases" : mode == "scheduler" ? "scheduled" : mode == "params" ? "parameter" :
                                mode == "output" ? "buffered output" : "quiet binary";
    std::vector<std::string> options{"benchmark", "--quiet", "--no-color", "--test", pattern, "--threads", mode == "scheduler" ? "4" : "1"};
    std::vector<char*> arguments;
    for (auto& option : options) arguments.push_back(option.data());
    std::size_t output_bytes = 0;
    chtest::set_output_sink([&](std::string_view text) { output_bytes += text.size(); });
    const auto start = std::chrono::steady_clock::now();
    const int result = chtest::run(static_cast<int>(arguments.size()), arguments.data());
    const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    chtest::clear_output_sink();
    std::cout << "mode=" << mode << " elapsed_ms=" << milliseconds << " parameter_copies=" << Parameter::copies
              << " parameter_copy_bytes=" << Parameter::copies * sizeof(Parameter) << " peak_memory_mib=" << peak_memory_mib()
              << " peak_cases=" << peak << " output_bytes=" << output_bytes << " checks=" << chtest::agg().checks << '\n';
    return result;
}
