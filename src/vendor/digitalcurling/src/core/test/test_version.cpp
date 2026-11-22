// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include "digitalcurling/version.hpp"

#include <string>

#include <gtest/gtest.h>

namespace dc = digitalcurling;

static_assert(dc::kLibraryVersionMajor == DIGITALCURLING_VERSION_MAJOR);
static_assert(dc::kLibraryVersionMinor == DIGITALCURLING_VERSION_MINOR);
static_assert(dc::kLibraryVersionPatch == DIGITALCURLING_VERSION_PATCH);

TEST(Version, LibraryVersionStringMatchesComponents)
{
    std::string const expected = std::to_string(dc::kLibraryVersionMajor) + "."
        + std::to_string(dc::kLibraryVersionMinor) + "."
        + std::to_string(dc::kLibraryVersionPatch);
    EXPECT_EQ(dc::kLibraryVersionString, expected);
    EXPECT_EQ(dc::kLibraryVersionString, DIGITALCURLING_VERSION_STRING);
}

TEST(Version, LibraryVersionMatchesProjectVersion)
{
    EXPECT_EQ(dc::kLibraryVersionString, DIGITALCURLING_TEST_EXPECTED_VERSION);
}
