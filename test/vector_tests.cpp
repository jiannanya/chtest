#include "chtest.hpp"

#include <list>
#include <string>
#include <tuple>
#include <vector>

TEST_CASE("vector push_back increases size") {
    std::vector<int> v;
    CHECK_EQ(v.size(), 0u);
    v.push_back(42);
    CHECK_EQ(v.size(), 1u);
    CHECK(1 + 1 == 2);

    SUBCASE("access element") {
        CHECK_NOTHROW(v.at(0));
        REQUIRE_EQ(v.at(0), 42);
    }

    SUBCASE("out_of_range throws") {
        CHECK_THROWS(v.at(1));
    }

    SUBCASE("out_of_range throws 2") {
        CHECK_THROWS(v.at(1));
    }
}

TEST_CASE("vector basics") {
    std::vector<int> v;
    CHECK(v.empty());

    SUBCASE("push_back") {
        v.push_back(42);
        REQUIRE(v.size() == 1);
    }

    SUBCASE("out_of_range") {
        CHECK_THROWS(v.at(1));
    }
}

TEST_CASE("vector basics 2") {
    std::vector<int> v;
    CHECK(v.empty());

    SUBCASE("push_back 2") {
        v.push_back(42);
        REQUIRE(v.size() == 1);
    }

    SUBCASE("out_of_range 2") {
        CHECK_THROWS(v.at(1));
    }
}

TEST_CASE("contains_and_size") {
    std::vector<std::string> names = {"alice", "bob", "carol"};
    CHECK_CONTAINS(names, std::string("bob"));
    CHECK_SIZE(names, 3);
}

TEST_CASE("seq_eq_lists_vectors") {
    std::vector<int> v = {1, 2, 3, 4};
    std::list<int> l = {1, 2, 3, 4};
    CHECK_SEQ_EQ(v, l);
}

TEST_CASE("seq_eq_mismatch_note") {
    std::vector<int> a = {1, 2, 4};
    std::list<int> b = {1, 2, 3};
    CHECK_SEQ_EQ(a, b);
}
