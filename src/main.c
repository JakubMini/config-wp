#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "application/config.h"
#include "drivers/storage.h"

#define APP_TASK_PRIORITY   ( tskIDLE_PRIORITY + 1 )
#define APP_TASK_STACK      ( configMINIMAL_STACK_SIZE * 2 )

static void prvAppTask( void * pvParameters )
{
    ( void ) pvParameters;

    storage_init();
    if( config_init() != CONFIG_OK )
    {
        printf( "config_init failed\n" );
    }

    for( ;; )
    {
        /* Candidates: replace this with whatever the configuration management
         * system needs to do at runtime (poll for updates, persist changes,
         * etc.). The task should never return. */
        printf( "app task running\n" );
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

static void prvShutdownHandler( int sig )
{
    ( void ) sig;
    _exit( 130 );
}

int main( void )
{
    /* The FreeRTOS POSIX port commandeers SIGUSR1 and SIGALRM. We only
     * install handlers for SIGINT/SIGTERM so Ctrl+C and `docker stop`
     * tear the process down cleanly. */
    signal( SIGINT,  prvShutdownHandler );
    signal( SIGTERM, prvShutdownHandler );

    BaseType_t rc = xTaskCreate(
        prvAppTask,
        "app",
        APP_TASK_STACK,
        NULL,
        APP_TASK_PRIORITY,
        NULL );

    if( rc != pdPASS )
    {
        printf( "failed to create app task\n" );
        return EXIT_FAILURE;
    }

    vTaskStartScheduler();

    /* Should never reach here. */
    for( ;; ) { }
    return EXIT_SUCCESS;
}

void vApplicationMallocFailedHook( void )
{
    printf( "ERROR: malloc failed\n" );
    abort();
}

void vAssertCalled( const char * const pcFileName, unsigned long ulLine )
{
    fprintf( stderr, "ASSERT: %s:%lu\n", pcFileName, ulLine );
    fflush( stderr );
    abort();
}

void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE * puxIdleTaskStackSize )
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];
    *ppxIdleTaskTCBBuffer    = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer  = uxIdleTaskStack;
    *puxIdleTaskStackSize    = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     configSTACK_DEPTH_TYPE * puxTimerTaskStackSize )
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t  uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];
    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *puxTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}
