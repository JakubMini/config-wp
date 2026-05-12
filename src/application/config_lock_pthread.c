/*****************************************************************************
 * Module:  config_lock_pthread
 * Purpose: pthread-backed implementation of the config_lock interface.
 *          Selected by CMake for host test builds where FreeRTOS isn't
 *          linked in. Single static pthread_mutex_t guarded by an init
 *          flag.
 *
 *          The "do I hold the lock" diagnostic uses C11 _Thread_local
 *          storage so each thread reads and writes only its own flag.
 *          No cross-thread reads, no torn-state race: a defensive assert
 *          like assert(!config_lock_is_held_by_current_thread()) is
 *          accurate even under heavy contention with many readers
 *          interleaving lock take/give around the asserting thread.
 *****************************************************************************/

#include "application/config_lock.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>

static pthread_mutex_t s_mutex;
static bool            s_initialised = false;

/* Per-thread flag, set inside the locked region and cleared just before
 * unlocking. Each thread sees only its own value — that's the property
 * that makes the is_held helper race-free without atomics. */
static _Thread_local bool t_this_thread_holds = false;

bool
config_lock_create (void)
{
    if (s_initialised)
    {
        return true;
    }
    if (pthread_mutex_init(&s_mutex, NULL) != 0)
    {
        return false;
    }
    s_initialised = true;
    return true;
}

void
config_lock_take (void)
{
    assert(s_initialised && "config_lock_create must be called first");
    (void)pthread_mutex_lock(&s_mutex);
    t_this_thread_holds = true;
}

void
config_lock_give (void)
{
    assert(s_initialised);
    t_this_thread_holds = false;
    (void)pthread_mutex_unlock(&s_mutex);
}

void
config_lock_destroy (void)
{
    if (!s_initialised)
    {
        return;
    }
    (void)pthread_mutex_destroy(&s_mutex);
    s_initialised = false;
    /* t_this_thread_holds is per-thread; this thread's flag clears via
     * the lock_give at the end of any successful take/give pair. Other
     * threads' flags are their problem (and should already be false in
     * a well-formed shutdown). */
}

bool
config_lock_is_held_by_current_thread (void)
{
    return t_this_thread_holds;
}
