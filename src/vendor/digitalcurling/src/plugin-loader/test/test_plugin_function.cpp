// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cstdlib>
#include <optional>

#include <gtest/gtest.h>

#include "digitalcurling/plugins/i_plugin_object.hpp"
#include "digitalcurling/plugins/plugin_api.hpp"
#include "digitalcurling/plugins/detail/plugin_resource.hpp"
#include "digitalcurling/plugins/detail/plugin_function.hpp"
#include "digitalcurling/plugins/detail/plugin_instance_list.hpp"

namespace {

using namespace digitalcurling::plugins;
using namespace digitalcurling::plugins::detail;

struct CountingFactory final : FactoryHandle {};

int g_destroy_count = 0;
bool g_create_succeeds = true;

void FreeString(char* text) { std::free(text); }

void DestroyHandle(void* handle) {
    ++g_destroy_count;
    delete static_cast<FactoryHandle*>(handle);
}

DigitalCurling_ErrorCode CreateFactory(const char*, FactoryHandle** out_handle, char** out_error) {
    *out_handle = new CountingFactory();
    *out_error = nullptr;
    return g_create_succeeds ? DIGITALCURLING_OK : DIGITALCURLING_ERR_INVALID_ARGUMENT;
}

class PluginFunctionCreatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_destroy_count = 0;
        g_create_succeeds = true;
    }
};

} // namespace

TEST_F(PluginFunctionCreatorTest, RegisteredHandleIsDestroyedExactlyOnce) {
    PluginInstanceList list;
    const PluginFunction<CreateFactoryFunc, digitalcurling::plugins::HandleId> create(&CreateFactory, &FreeString, &DestroyHandle, list);

    const auto result = create.ExecuteRaw("{}");
    ASSERT_TRUE(result);
    EXPECT_EQ(g_destroy_count, 0);
    EXPECT_TRUE(list.Exists(result.GetValue()));

    EXPECT_TRUE(list.Remove(result.GetValue()));
    EXPECT_EQ(g_destroy_count, 1);
}

TEST_F(PluginFunctionCreatorTest, FailedCreationDestroysReturnedHandleOnce) {
    g_create_succeeds = false;
    PluginInstanceList list;
    const PluginFunction<CreateFactoryFunc, digitalcurling::plugins::HandleId> create(&CreateFactory, &FreeString, &DestroyHandle, list);

    const auto result = create.ExecuteRaw("{}");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.GetError().code(), DIGITALCURLING_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(g_destroy_count, 1);
    EXPECT_TRUE(list.GetAllId().empty());
}

TEST(PluginInstanceList, AddReturnsDistinctIds) {
    PluginInstanceList list;
    const auto first = list.Add(std::make_shared<CountingFactory>());
    const auto second = list.Add(std::make_shared<CountingFactory>());
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first.value(), second.value());
    EXPECT_EQ(list.GetAllId().size(), 2u);
}
