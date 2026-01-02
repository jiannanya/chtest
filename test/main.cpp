#include "chtest.hpp"

#include "test_env.hpp"

int main(int argc, char** argv) {
    CHTEST_SET_ENV(g_env);
    return chtest::run(argc, argv);
}
