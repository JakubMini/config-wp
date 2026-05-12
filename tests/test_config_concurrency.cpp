/*****************************************************************************
 * Module:  test_config_concurrency
 * Purpose: Stress-tests the manager's threading discipline. Three pieces:
 *
 *            1. Lock-state property tests — config_lock_is_held_by_current
 *               _thread returns true only inside the locked region of the
 *               calling thread.
 *            2. Reader/writer stress — N reader threads + 1 writer thread
 *               hammer the cache for ~2 s; readers verify a self-consistent
 *               invariant on every record they read (id == debounce_ms,
 *               because both come from the same write counter). Any torn
 *               read fails the test.
 *            3. Save-during-reads — writer thread calls config_save while
 *               readers continue. The priority-inversion guard
 *               (assert(!lock_held) before slot_write) and the readers'
 *               own progress prove the save path doesn't block readers
 *               on the storage I/O.
 *
 *          Run with --gtest_repeat=until-fail:50 in CI to maximise the
 *          chance of catching rare ordering hazards.
 *****************************************************************************/

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include "application/config.h"
#include "application/config_defaults.h"
#include "application/config_lock.h"
#include "drivers/storage.h"
}

namespace {

constexpr int          kReaderCount    = 4;
constexpr auto         kStressDuration = std::chrono::seconds(2);
constexpr unsigned int kMinReads       = 1000u;
constexpr unsigned int kMinWrites      = 100u;

} // namespace

/* =================================================================
 * Lock-state property tests
 * ================================================================= */

class LockStateTest : public ::testing::Test
{
protected:
    void SetUp () override
    {
        config_deinit();
        ASSERT_TRUE(config_lock_create());
    }
    void TearDown () override
    {
        config_lock_destroy();
    }
};

TEST_F(LockStateTest, NotHeldByDefault)
{
    EXPECT_FALSE(config_lock_is_held_by_current_thread());
}

TEST_F(LockStateTest, HeldOnlyInsideLockedRegion)
{
    EXPECT_FALSE(config_lock_is_held_by_current_thread());
    config_lock_take();
    EXPECT_TRUE(config_lock_is_held_by_current_thread());
    config_lock_give();
    EXPECT_FALSE(config_lock_is_held_by_current_thread());
}

TEST_F(LockStateTest, NotHeldByOtherThread)
{
    config_lock_take();

    std::atomic<bool> child_observed_held { false };
    std::thread       child([&] {
        child_observed_held.store(config_lock_is_held_by_current_thread(),
                                  std::memory_order_release);
    });
    child.join();

    EXPECT_TRUE(config_lock_is_held_by_current_thread());
    EXPECT_FALSE(child_observed_held.load(std::memory_order_acquire))
        << "another thread must not see itself as the lock holder";

    config_lock_give();
}

/* =================================================================
 * Reader/writer stress
 * ================================================================= */

class ConcurrencyTest : public ::testing::Test
{
protected:
    void SetUp () override
    {
        config_deinit();
        ASSERT_EQ(storage_init(), STORAGE_OK);
        ASSERT_EQ(config_init(), CONFIG_OK);
    }
    void TearDown () override
    {
        config_deinit();
    }
};

TEST_F(ConcurrencyTest, ReadersAndWriterNoTorn)
{
    /* The writer keeps incrementing a counter and stamps it into di[0]
     * via two distinct fields (id and debounce_ms). Both are written
     * atomically under the manager's mutex. A reader that sees those
     * two fields disagree is observing a torn read — proof the mutex
     * isn't actually serialising. */
    /* Pre-stamp di[0] so id == debounce_ms BEFORE any reader starts.
     * The defaults table has id=0 and debounce_ms=10 — different
     * values. Without this seed, a reader that races ahead of the
     * writer's first config_set_di would observe the (consistent but
     * unequal) default state and the test's "id == debounce_ms"
     * invariant would flag it as a torn read. */
    {
        di_config_t seed = g_di_defaults[0];
        seed.id          = 0;
        seed.debounce_ms = 0;
        ASSERT_EQ(config_set_di(0, &seed), CONFIG_OK);
    }

    std::atomic<bool>     stop { false };
    std::atomic<uint64_t> read_count { 0 };
    std::atomic<uint64_t> write_count { 0 };
    std::atomic<uint64_t> torn_reads { 0 };
    std::atomic<uint64_t> errors { 0 };

    std::thread writer([&] {
        uint32_t counter = 0;
        while (!stop.load(std::memory_order_acquire))
        {
            di_config_t    di       = g_di_defaults[0];
            const uint16_t stamp    = static_cast<uint16_t>(counter & 0xFFFFu);
            di.id                   = stamp;
            di.debounce_ms          = stamp;
            const config_status_t s = config_set_di(0, &di);
            if (s != CONFIG_OK)
            {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                write_count.fetch_add(1, std::memory_order_relaxed);
            }
            counter++;
        }
    });

    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);
    for (int i = 0; i < kReaderCount; ++i)
    {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                di_config_t           di;
                const config_status_t s = config_get_di(0, &di);
                if (s != CONFIG_OK)
                {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (di.id != di.debounce_ms)
                {
                    torn_reads.fetch_add(1, std::memory_order_relaxed);
                }
                read_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(kStressDuration);
    stop.store(true, std::memory_order_release);
    writer.join();
    for (auto & t : readers)
    {
        t.join();
    }

    EXPECT_EQ(errors.load(), 0u);
    EXPECT_EQ(torn_reads.load(), 0u)
        << "observed " << torn_reads.load() << " torn reads across "
        << read_count.load() << " total reads and " << write_count.load()
        << " writes";
    EXPECT_GT(read_count.load(), static_cast<uint64_t>(kMinReads))
        << "readers ran too few iterations — test didn't actually stress";
    EXPECT_GT(write_count.load(), static_cast<uint64_t>(kMinWrites))
        << "writer ran too few iterations — test didn't actually stress";
}

TEST_F(ConcurrencyTest, SaveDoesNotBlockReadersForever)
{
    /* config_save releases the cache mutex before calling slot_write, so
     * readers should make forward progress even while a save is in
     * flight. The priority-inversion guard (assert(!lock_held) before
     * slot_write) covers the assertion side; this test covers the
     * observable side. */
    std::atomic<bool>     stop { false };
    std::atomic<uint64_t> read_count { 0 };
    std::atomic<uint64_t> save_count { 0 };
    std::atomic<uint64_t> errors { 0 };

    std::thread saver([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            const config_status_t s = config_save();
            if (s != CONFIG_OK)
            {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            save_count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);
    for (int i = 0; i < kReaderCount; ++i)
    {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                system_config_t       sys;
                const config_status_t s = config_get_system(&sys);
                if (s != CONFIG_OK)
                {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
                read_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(kStressDuration);
    stop.store(true, std::memory_order_release);
    saver.join();
    for (auto & t : readers)
    {
        t.join();
    }

    EXPECT_EQ(errors.load(), 0u);
    EXPECT_GT(read_count.load(), static_cast<uint64_t>(kMinReads))
        << "readers made too little progress during concurrent saves — "
        << "the save path may be holding the lock across storage I/O";
    EXPECT_GT(save_count.load(), 0u);
}
