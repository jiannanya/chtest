#include "chtest.hpp"

#include <tuple>
#include <vector>

TEST_CASE("shuffle and repeat demonstration") {
    CHECK(1 + 1 == 2);
}

TEST_CASE("check equal example") {
    int a = 2;
    CHECK_EQ(a, 2);
}

static auto params = std::vector<std::tuple<int, int, int>>{{1, 2, 3}, {5, 7, 12}, {0, 0, 0}};

TEST_CASE_PARAM("addition param", params) {
    auto [a, b, expected] = param;
    CHECK_EQ(a + b, expected);
}
