/*****************************************************************************
 * Module:  config_lock_freertos
 * Purpose: FreeRTOS-backed implementation of the config_lock interface.
 *          Selected by CMake when BUILD_APP=ON. Uses xSemaphoreCreateMutex
 *          for a non-recursive priority-inheriting mutex.
 *****************************************************************************/

#include "application/config_lock.h"

#include "FreeRTOS.h"
#include "semphr.h"

#include <assert.h>
#include <stdbool.h>

static SemaphoreHandle_t s_mutex = NULL;

bool
config_lock_create (void)
{
    if (s_mutex != NULL)
    {
        return true;
    }
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

void
config_lock_take (void)
{
    assert(s_mutex != NULL && "config_lock_create must be called first");
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void
config_lock_give (void)
{
    assert(s_mutex != NULL);
    (void)xSemaphoreGive(s_mutex);
}

void
config_lock_destroy (void)
{
    if (s_mutex == NULL)
    {
        return;
    }
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;
}
