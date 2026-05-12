/*****************************************************************************
 * Module:  external_comms
 * Purpose: Implementation of the External Comms task — see header for
 *          the architectural contract.
 *
 *          The task body is deliberately minimal: receive a {pointer,
 *          length} record, call config_import_json, log the report.
 *          All validation, mutex discipline, and partial-update
 *          semantics live one layer down in the manager and the JSON
 *          wrapper. This layer is purely a router.
 *****************************************************************************/

#include "external_comms.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "application/config.h"
#include "application/config_json.h"
#include "application/eeprom_manager.h"

#include "config_print.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define EXT_TASK_PRIORITY  (tskIDLE_PRIORITY + 1)
#define EXT_TASK_STACK     (configMINIMAL_STACK_SIZE * 4U) /* cJSON heap */
#define EXT_QUEUE_DEPTH    4U
#define EXT_SUBMIT_TIMEOUT pdMS_TO_TICKS(100)

typedef struct
{
    const char * json;
    size_t       len;
} ext_msg_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task  = NULL;

static void
ext_log_report (config_status_t st, const config_import_report_t * rep)
{
    assert(rep != NULL);
    printf(
        "[ext] config_import_json -> %s "
        "(accepted=%u rejected=%u unknown_keys=%u malformed=%u)\n",
        config_print_status(st),
        (unsigned)rep->accepted,
        (unsigned)rep->rejected,
        (unsigned)rep->unknown_keys,
        (unsigned)rep->malformed);
    if (rep->first_error[0] != '\0')
    {
        printf("[ext] first_error: %s\n", rep->first_error);
    }
}

static void
ext_task (void * pv)
{
    (void)pv;
    assert(s_queue != NULL);

    for (;;)
    {
        ext_msg_t msg = { .json = NULL, .len = 0u };
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdPASS)
        {
            continue;
        }
        if (msg.json == NULL || msg.len == 0u)
        {
            continue;
        }

        config_import_report_t rep = { 0 };
        printf("[ext] importing JSON patch (%zu bytes)...\n", msg.len);
        config_status_t st = config_import_json(msg.json, msg.len, &rep);
        ext_log_report(st, &rep);

        /* Persistence is the EEPROM Manager's job. We just signal that
         * the cache changed; the consumer task chooses when/how to
         * commit (debounce, coalesce, gate on power-OK, etc.). Partial
         * accepts (some records rejected) still count — the manager
         * already filtered them out, so what's in RAM is consistent
         * and worth eventually persisting. */
        if (st == CONFIG_OK)
        {
            const bool queued = QueueConfigCommit();
            printf("[ext] QueueConfigCommit -> %s\n",
                   queued ? "queued" : "FAIL");
        }
        else
        {
            printf("[ext] skipping commit (import not OK)\n");
        }
    }
}

void
external_comms_init (void)
{
    if (s_queue != NULL)
    {
        return; /* idempotent */
    }
    s_queue = xQueueCreate(EXT_QUEUE_DEPTH, sizeof(ext_msg_t));
    configASSERT(s_queue != NULL);

    BaseType_t rc = xTaskCreate(ext_task,
                                "ext-comms",
                                EXT_TASK_STACK,
                                NULL,
                                EXT_TASK_PRIORITY,
                                &s_task);
    configASSERT(rc == pdPASS);
    (void)rc;
}

bool
external_comms_submit (const char * json, size_t len)
{
    if (json == NULL || len == 0u || s_queue == NULL)
    {
        return false;
    }
    ext_msg_t msg = { .json = json, .len = len };
    return xQueueSend(s_queue, &msg, EXT_SUBMIT_TIMEOUT) == pdPASS;
}
