#pragma once

#include "chtest.hpp"

struct GlobalState {
    static thread_local int counter;
};

struct ResetEnv : chtest::Environment {
    void setUp() override;
    void tearDown() override;
};

extern ResetEnv g_env;
