#include "chtest.hpp"

#include <string>
#include <vector>

static auto tags = std::vector<std::string>{"fast", "math", "unit"};

TEST_CASE_TAG("test tags", {tags}) {
    CHECK_EQ(2 + 2, 4);
}

static auto tags2 = std::vector<std::string>{"fast", "math", "go"};

TEST_CASE_TAG("test tags 2", {tags2}) {
    CHECK_EQ(2 + 2, 4);
}
