#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "application/config.h"
#include "application/eeprom_manager.h"
#include "drivers/uart_sim.h"

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

    config_print_stage("stage 1: cache populated by EEPROM Manager at boot");
    /* eeprom_manager_init() ran before the scheduler started; it did
     * storage_init + config_init, so the cache is already live. */

    config_print_system("after init");
    config_print_di(0);
    config_print_di(1);

    /* --- mutate --- */
    config_print_stage("stage 2: mutate one DI + system, then save");

    config_status_t st;

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

    /* Persistence goes through the EEPROM Manager queue now — direct
     * config_save() from a second task would race the manager's own
     * save call and break the single-writer contract. */
    const bool committed = QueueConfigCommit();
    printf("[app] QueueConfigCommit -> %s\n", committed ? "queued" : "FAIL");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* --- piecewise change via QueueConfigChange --- */
    config_print_stage(
        "stage 3: piecewise field change via QueueConfigChange API");

    eeprom_value_t v  = { .u = 75 };
    bool           ok = QueueConfigChange(EEPROM_TYPE_IO_DI,
                                /*item=*/0,
                                /*param=*/DI_PARAM_DEBOUNCE_MS,
                                v);
    printf("[app] QueueConfigChange(di[0].debounce_ms=75) -> %s\n",
           ok ? "queued" : "FAIL");

    v.u = 17;
    ok  = QueueConfigChange(EEPROM_TYPE_SYSTEM,
                           /*item=*/0,
                           /*param=*/SYSTEM_PARAM_CANOPEN_NODE_ID,
                           v);
    printf("[app] QueueConfigChange(system.canopen_node_id=17) -> %s\n",
           ok ? "queued" : "FAIL");

    /* Let the manager coalesce + save (one slot write for both). */
    vTaskDelay(pdMS_TO_TICKS(100));
    config_print_di(0);
    config_print_system("after piecewise");

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

    /* The comms task auto-saves on successful import, so no explicit
     * config_save() is needed here — stage 3's reload demo already
     * proved the storage round-trip works. */

    /* --- idle loop --- */
    config_print_stage("idle: demo complete, task tick once a second");

    for (;;)
    {
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

    /* EEPROM Manager runs first on boot: synchronously calls config_init
     * so the cache is populated before any other task starts, then
     * spawns the worker task that owns config_save going forward. */
    config_status_t mst = eeprom_manager_init();
    printf("[boot] eeprom_manager_init -> %s\n", config_print_status(mst));

    /* Spawn the external comms task + queue ahead of the scheduler so
     * the consumer is ready before any producer submits. */
    external_comms_init();

    /* Spawn the simulated-UART listener (TCP :5555). */
    uart_sim_init(5555);

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
