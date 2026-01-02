#include "chtest.hpp"

#include "test_env.hpp"

struct MyFixture {
    int x = 0;
    void setUp() { x = 7; }
    void tearDown() { x = -1; }
};

TEST_F(MyFixture, "fixture increments") {
    CHECK_EQ(x, 7);
    CHECK_EQ(x, 7);
    x++;
    CHECK_EQ(x, 8);
    x++;
    REQUIRE_EQ(x, 9);
}

TEST_CASE("counter test 1") {
    GlobalState::counter++;
    CHECK_EQ(GlobalState::counter, 1);
}

TEST_CASE("another test 5") {
    GlobalState::counter += 5;
    CHECK_EQ(GlobalState::counter, 5);
}
