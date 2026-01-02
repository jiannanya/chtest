#include "test_env.hpp"

int GlobalState::counter = 0;

void ResetEnv::setUp() {
    GlobalState::counter = 0;
}

void ResetEnv::tearDown() {
    // optional cleanup
}

ResetEnv g_env;
