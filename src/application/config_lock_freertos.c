/*****************************************************************************
 * Module:  config_lock_freertos
 * Purpose: FreeRTOS-backed implementation of the config_lock interface.
 *          Selected by CMake when BUILD_APP=ON. Uses xSemaphoreCreateMutex
 *          for a non-recursive priority-inheriting mutex.
 *
 *          Tracks the owner task explicitly so
 *          config_lock_is_held_by_current_thread() works without relying
 *          on the optional xSemaphoreGetMutexHolder API (which requires
 *          INCLUDE_xSemaphoreGetMutexHolder in FreeRTOSConfig.h).
 *****************************************************************************/

#include "application/config_lock.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include <assert.h>
#include <stdbool.h>

static SemaphoreHandle_t s_mutex     = NULL;
static TaskHandle_t      s_owner     = NULL;
static bool              s_has_owner = false;

bool
config_lock_create (void)
{
    if (s_mutex != NULL)
    {
        return true;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL)
    {
        return false;
    }
    s_has_owner = false;
    return true;
}

void
config_lock_take (void)
{
    assert(s_mutex != NULL && "config_lock_create must be called first");
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_owner     = xTaskGetCurrentTaskHandle();
    s_has_owner = true;
}

void
config_lock_give (void)
{
    assert(s_mutex != NULL);
    s_has_owner = false;
    (void)xSemaphoreGive(s_mutex);
}

void
config_lock_destroy (void)
{
    if (s_mutex == NULL)
    {
        return;
    }
    s_has_owner = false;
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
}

bool
config_lock_is_held_by_current_thread (void)
{
    if (s_mutex == NULL || !s_has_owner)
    {
        return false;
    }
    return s_owner == xTaskGetCurrentTaskHandle();
}
