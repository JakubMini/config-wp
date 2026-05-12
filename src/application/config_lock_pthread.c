/*****************************************************************************
 * Module:  config_lock_pthread
 * Purpose: pthread-backed implementation of the config_lock interface.
 *          Selected by CMake for host test builds where FreeRTOS isn't
 *          linked in. The implementation is intentionally trivial — a
 *          single static pthread_mutex_t guarded by an init flag.
 *****************************************************************************/

#include "application/config_lock.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>

static pthread_mutex_t s_mutex;
static bool            s_initialised = false;

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
}

void
config_lock_give (void)
{
    assert(s_initialised);
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
}
