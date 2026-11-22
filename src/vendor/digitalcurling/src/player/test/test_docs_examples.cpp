// Loads the JSON examples printed in docs/players.md (kept as fixture files)
// through the player factories to keep the documentation loadable.

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "common.hpp"

#ifdef DIGITALCURLING_TEST_DOCS_PLAYER_IDENTICAL
#include "../src/identical/player_identical_factory.hpp"
#endif
#ifdef DIGITALCURLING_TEST_DOCS_PLAYER_NORMAL_DIST
#include "../src/normal_dist/player_normal_dist_factory.hpp"
#endif
#ifdef DIGITALCURLING_TEST_DOCS_PLAYER_ND2
#include "../src/nd2/player_nd2_factory.hpp"
#endif

namespace {

json LoadDocsFixture(std::string const& name)
{
    std::string const path = std::string(DIGITALCURLING_TEST_DOCS_FIXTURE_DIR) + "/" + name;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open docs fixture: " + path);
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return json::parse(buffer.str());
}

} // namespace

#ifdef DIGITALCURLING_TEST_DOCS_PLAYER_IDENTICAL
TEST(DocsExamples, PlayerIdentical)
{
    json const j = LoadDocsFixture("identical.json");
    dcp::PlayerIdenticalFactory factory;
    EXPECT_NO_THROW(j.get_to(factory));
    EXPECT_EQ(factory.gender, dcp::Gender::kMale);
}
#endif

#ifdef DIGITALCURLING_TEST_DOCS_PLAYER_NORMAL_DIST
TEST(DocsExamples, PlayerNormalDist)
{
    json const j = LoadDocsFixture("normal_dist.json");
    dcp::PlayerNormalDistFactory factory;
    EXPECT_NO_THROW(j.get_to(factory));
    EXPECT_EQ(factory.gender, dcp::Gender::kMale);
    EXPECT_FALSE(factory.seed.has_value());
}
#endif

#ifdef DIGITALCURLING_TEST_DOCS_PLAYER_ND2
TEST(DocsExamples, PlayerND2)
{
    json const j = LoadDocsFixture("nd2.json");
    dcp::PlayerND2Factory factory;
    EXPECT_NO_THROW(j.get_to(factory));
    EXPECT_EQ(factory.gender, dcp::Gender::kFemale);
    EXPECT_FALSE(factory.seed.has_value());
}
#endif