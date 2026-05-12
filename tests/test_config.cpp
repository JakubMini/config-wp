/*****************************************************************************
 * Module:  test_config
 * Purpose: Baseline tests for the configuration manager. Expanded as the
 *          API grows; for now this only proves that init is idempotent and
 *          that the stub getters/setters behave as documented.
 *****************************************************************************/

#include <gtest/gtest.h>
#include <gmock/gmock.h>

extern "C" {
#include "application/config.h"
#include "drivers/storage.h"
}

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(storage_init(), STORAGE_OK);
        ASSERT_EQ(config_init(), CONFIG_OK);
    }
};

TEST_F(ConfigTest, InitIsIdempotent) {
    EXPECT_EQ(config_init(), CONFIG_OK);
    EXPECT_EQ(config_init(), CONFIG_OK);
}

TEST_F(ConfigTest, StubGetReturnsOK) {
    EXPECT_EQ(config_get(), CONFIG_OK);
}

TEST_F(ConfigTest, StubSetReturnsOK) {
    EXPECT_EQ(config_set(), CONFIG_OK);
}
