#include "test_env.hpp"

thread_local int GlobalState::counter = 0;

void ResetEnv::setUp() {
    GlobalState::counter = 0;
}

void ResetEnv::tearDown() {
    // optional cleanup
}

ResetEnv g_env;
