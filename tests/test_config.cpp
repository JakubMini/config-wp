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

TEST_F(ConfigTest, GetDemoOK) {
    EXPECT_EQ(config_get(), CONFIG_OK);
}

// Failing test
TEST_F(ConfigTest, GetDemoFail) {
    EXPECT_EQ(config_get(), CONFIG_ERR_NOT_FOUND);
}
