#include <chtest.hpp>
TEST_CASE_TAG("installed header second translation unit", {"package", "smoke"}) {
    auto worker = chtest::spawn_with_context([] { REQUIRE(true); });
    worker.join();
}
