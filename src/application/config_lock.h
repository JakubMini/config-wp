/*****************************************************************************
 * Module:  config_lock
 * Purpose: Abstract mutex used by the configuration manager to serialise
 *          access to the in-RAM cache. Two implementations:
 *
 *            - config_lock_freertos.c  wraps SemaphoreHandle_t when the
 *                                      FreeRTOS app target is built.
 *            - config_lock_pthread.c   wraps pthread_mutex_t for host
 *                                      test builds where FreeRTOS isn't
 *                                      linked.
 *
 *          CMake picks one implementation based on BUILD_APP. The public
 *          interface here is identical in both worlds so the manager code
 *          is byte-identical across builds.
 *
 *          The lock is module-static — a single global mutex owned by
 *          this module. The manager treats it as non-recursive: a public
 *          API function must never call another public API function
 *          while holding the lock.
 *****************************************************************************/

#ifndef APPLICATION_CONFIG_LOCK_H
#define APPLICATION_CONFIG_LOCK_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>

/* Initialise the module-static lock. Safe to call more than once
 * (idempotent — second and subsequent calls return true without
 * creating a new lock). Returns false only if the underlying OS
 * primitive fails to allocate / create. */
bool config_lock_create (void);

/* Acquire the lock. Blocks until available. */
void config_lock_take (void);

/* Release the lock. */
void config_lock_give (void);

/* Tear down the lock. Used primarily by tests to reset module state
 * between cases. Safe to call when no lock exists. */
void config_lock_destroy (void);

/* Returns true iff this thread currently holds the lock. Intended for
 * defensive assertions (e.g. "no SPI I/O while the cache mutex is
 * held").
 *
 * The pthread backend uses a C11 _Thread_local flag — each thread
 * reads and writes only its own state, so the return value is accurate
 * even under heavy contention.
 *
 * The FreeRTOS backend tracks the holder via a single static
 * TaskHandle_t; that has a narrow TOCTOU window during concurrent
 * take/give from another task. The result is "almost always right" and
 * fine as a defensive assert in single-writer paths (config_save is
 * one), but should not be used as a synchronisation primitive. */
bool config_lock_is_held_by_current_thread (void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_LOCK_H */
