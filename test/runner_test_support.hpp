#pragma once
#include "chtest.hpp"

// These checks intentionally do not use the assertion machinery under test.
namespace verification {
inline std::uint64_t checked = 0;
inline std::string output;
inline void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checked;
}
inline void reset() {
    chtest::registry().clear();
    chtest::global_env() = nullptr;
    chtest::reset_aggregates();
    output.clear();
    chtest::set_output_sink([](std::string_view text) { output.append(text); });
}
inline void add(std::string name, std::function<void()> body, int retries = 0,
                std::function<bool()> skip = {}) {
    chtest::registry().push_back({std::move(name), std::move(body), {}, false, {}, 0, retries, std::move(skip)});
}
inline int run(std::initializer_list<const char*> options = {}) {
    std::vector<std::string> args{"verification", "--quiet", "--no-color"};
    for (const char* option : options) args.emplace_back(option);
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    output.clear();
    return chtest::run(static_cast<int>(argv.size()), argv.data());
}
template <typename F> int main(const char* name, F&& verify) {
    try {
        verify();
        reset();
        chtest::clear_output_sink();
        std::cout << name << ": " << checked << " independent checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        chtest::clear_output_sink();
        std::cerr << name << " FAILED: " << error.what() << "\nLast output:\n" << output;
        return 1;
    }
}
}
