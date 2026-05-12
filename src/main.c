#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "application/config.h"
#include "application/config_json.h"
#include "drivers/storage.h"

#include "config_print.h"
#include "external_comms.h"

#define APP_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define APP_TASK_STACK    (configMINIMAL_STACK_SIZE * 2)

/* ===================================================================
 * Demo flow
 * =================================================================== */

static void
prvAppTask (void * pvParameters)
{
    (void)pvParameters;

    config_print_stage("stage 1: storage + config manager init");
    storage_status_t sst = storage_init();
    printf("[drv] storage_init -> %s\n", sst == STORAGE_OK ? "OK" : "ERR");
    if (sst != STORAGE_OK)
    {
        /* No storage means config_init will fall back to defaults via the
         * SLOT_ERR_STORAGE path. Demo continues so we can still show the
         * defaults flow, but a real device would probably halt or fault. */
        printf("[drv] continuing on defaults — slot reads will fail\n");
    }
    config_status_t st = config_init();
    printf("[cfg] config_init -> %s\n", config_print_status(st));

    config_print_system("after init");
    config_print_di(0);
    config_print_di(1);

    /* --- mutate --- */
    config_print_stage("stage 2: mutate one DI + system, then save");

    di_config_t di0;
    if (config_get_di(0, &di0) == CONFIG_OK)
    {
        snprintf(di0.name, sizeof(di0.name), "demo-di");
        di0.debounce_ms       = 25;
        di0.polarity          = DI_POLARITY_ACTIVE_LOW;
        di0.interrupt_enabled = true;
        st                    = config_set_di(0, &di0);
        printf("[cfg] config_set_di(0, demo-di) -> %s\n",
               config_print_status(st));
    }

    system_config_t sys;
    if (config_get_system(&sys) == CONFIG_OK)
    {
        sys.canopen_node_id = 42;
        sys.heartbeat_ms    = 500;
        st                  = config_set_system(&sys);
        printf("[cfg] config_set_system(node=42, hb=500) -> %s\n",
               config_print_status(st));
    }

    config_print_di(0);
    config_print_system("after mutation");

    st = config_save();
    printf("\n[cfg] config_save -> %s\n", config_print_status(st));

    /* --- reload --- */
    config_print_stage("stage 3: reload from storage (simulates power cycle)");

    config_deinit();
    st = config_init();
    printf("[cfg] config_init -> %s\n", config_print_status(st));

    config_print_di(0);
    config_print_system("after reload");

    /* --- reset --- */
    config_print_stage(
        "stage 4: reset to factory defaults (in-RAM only, not persisted)");

    st = config_reset_defaults();
    printf("[cfg] config_reset_defaults -> %s\n", config_print_status(st));

    config_print_di(0);
    config_print_system("after reset");

    /* --- json patch via external comms task --- */
    config_print_stage("stage 5: submit JSON patch via external comms thread");

    /* Operator-style patch: one record, addressed by channel, carrying
     * only the field that changes. The buffer lives in BSS so it stays
     * valid past the submit call — the comms task may not pick it up
     * until we yield. */
    static const char patch_json[]
        = "{\n"
          "  \"//note\": \"demo: bump di[ch=9] debounce only\",\n"
          "  \"di\": [ { \"channel\": 9, \"debounce_ms\": 100 } ]\n"
          "}\n";

    config_print_di(9);

    const bool submitted
        = external_comms_submit(patch_json, sizeof(patch_json) - 1U);
    printf("[app] external_comms_submit -> %s\n",
           submitted ? "queued" : "FAIL");

    /* Yield so the comms task can drain. Both tasks run at IDLE+1, so
     * a short delay is enough; in a real device the comms task would
     * sit at a low priority and naturally pre-empt nothing. */
    vTaskDelay(pdMS_TO_TICKS(100));

    config_print_di(9);

    /* Persistence is a separate concern from import. The comms task
     * never auto-saves — we commit explicitly after a batch of patches. */
    st = config_save();
    printf(
        "\n[cfg] config_save -> %s "
        "(committed external patches to flash)\n",
        config_print_status(st));

    /* --- json export --- */
    config_print_stage("stage 6: export current cache as JSON");

    static char export_buf[8192];
    size_t      written = 0;
    st = config_export_json(export_buf, sizeof(export_buf), &written);
    printf("[json] config_export_json -> %s (%zu bytes)\n",
           config_print_status(st),
           written);
    if (st == CONFIG_OK)
    {
        fputs(export_buf, stdout);
        fputc('\n', stdout);
    }

    /* --- idle loop --- */
    config_print_stage("idle: demo complete, task tick once a second");

    unsigned tick = 0;
    for (;;)
    {
        printf("[app] idle tick %u\n", tick++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void
prvShutdownHandler (int sig)
{
    (void)sig;
    _exit(130);
}

int
main (void)
{
    /* The FreeRTOS POSIX port commandeers SIGUSR1 and SIGALRM. We only
     * install handlers for SIGINT/SIGTERM so Ctrl+C and `docker stop`
     * tear the process down cleanly. */
    signal(SIGINT, prvShutdownHandler);
    signal(SIGTERM, prvShutdownHandler);

    /* Spawn the external comms task + queue ahead of the scheduler so
     * the consumer is ready before any producer submits. */
    external_comms_init();

    BaseType_t rc = xTaskCreate(
        prvAppTask, "app", APP_TASK_STACK, NULL, APP_TASK_PRIORITY, NULL);

    if (rc != pdPASS)
    {
        printf("failed to create app task\n");
        return EXIT_FAILURE;
    }

    vTaskStartScheduler();

    /* Should never reach here. */
    for (;;)
    {
    }
    return EXIT_SUCCESS;
}

void
vApplicationMallocFailedHook (void)
{
    printf("ERROR: malloc failed\n");
    abort();
}

void
vAssertCalled (const char * const pcFileName, unsigned long ulLine)
{
    fprintf(stderr, "ASSERT: %s:%lu\n", pcFileName, ulLine);
    fflush(stderr);
    abort();
}

void
vApplicationGetIdleTaskMemory (StaticTask_t **          ppxIdleTaskTCBBuffer,
                               StackType_t **           ppxIdleTaskStackBuffer,
                               configSTACK_DEPTH_TYPE * puxIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  uxIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *puxIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

void
vApplicationGetTimerTaskMemory (StaticTask_t ** ppxTimerTaskTCBBuffer,
                                StackType_t **  ppxTimerTaskStackBuffer,
                                configSTACK_DEPTH_TYPE * puxTimerTaskStackSize)
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t  uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];
    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *puxTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}
