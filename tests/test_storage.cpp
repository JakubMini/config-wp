#include <gtest/gtest.h>

extern "C"
{
#include "drivers/storage.h"
}

TEST(StorageTest, ReadAfterWriteReturnsSameBytes)
{
    ASSERT_EQ(storage_init(), STORAGE_OK);

    const uint8_t in[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_EQ(storage_write(0, in, sizeof(in)), STORAGE_OK);

    uint8_t out[sizeof(in)] = {};
    ASSERT_EQ(storage_read(0, out, sizeof(out)), STORAGE_OK);
    EXPECT_EQ(0, memcmp(in, out, sizeof(in)));
}

TEST(StorageTest, OutOfRangeWriteRejected)
{
    ASSERT_EQ(storage_init(), STORAGE_OK);

    uint8_t b = 0;
    EXPECT_EQ(storage_write(static_cast<uint32_t>(storage_size()), &b, 1),
              STORAGE_ERR_RANGE);
}
