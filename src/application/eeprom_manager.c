/*****************************************************************************
 * Module:  eeprom_manager
 * Purpose: Implementation of the EEPROM Manager task. See header for
 *          the architectural contract.
 *
 *          The task body is a classic producer-consumer drain:
 *              block on the queue,
 *              apply one request,
 *              opportunistically drain anything queued during apply,
 *              call config_save() once at the end.
 *          That coalescing matters for FRAM/EEPROM endurance — a burst
 *          of N piecewise updates becomes one slot write.
 *****************************************************************************/

#include "application/eeprom_manager.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "application/config.h"
#include "application/config_types.h"
#include "config_print.h"
#include "drivers/storage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EEPROM_TASK_PRIORITY (tskIDLE_PRIORITY + 1) /* "Low" per spec */
#define EEPROM_TASK_STACK    (configMINIMAL_STACK_SIZE * 4U)
#define EEPROM_QUEUE_DEPTH   16U
#define EEPROM_ENQ_TIMEOUT   pdMS_TO_TICKS(50)

typedef enum
{
    REQ_KIND_FIELD = 0,
    REQ_KIND_COMMIT,
} eeprom_req_kind_t;

typedef struct
{
    eeprom_req_kind_t    kind;
    eeprom_config_type_t type;  /* used only when kind == FIELD */
    uint8_t              item;  /* IO channel index; ignored for SYSTEM */
    uint16_t             param; /* per-type field id */
    eeprom_value_t       value;
} eeprom_req_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_task  = NULL;

/* ====================================================================
 * Per-type field appliers
 *
 * Each loads the current cache record, mutates the requested field,
 * and writes it back. We rely on config_set_<type>() to validate the
 * struct as a whole — out-of-range values come back as
 * CONFIG_ERR_INVALID and we log + drop, exactly the spec's "checks
 * input validity" requirement.
 * ==================================================================== */

static void
copy_name_locked (char * dst, const eeprom_value_t * v)
{
    /* NUL-terminate defensively; the union arm is bounded but a buggy
     * caller could pass an unterminated buffer. */
    memcpy(dst, v->s, CONFIG_NAME_LEN);
    dst[CONFIG_NAME_LEN - 1] = '\0';
}

static config_status_t
apply_di (uint8_t idx, uint16_t param, const eeprom_value_t * v)
{
    di_config_t     rec;
    config_status_t st = config_get_di(idx, &rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((di_param_t)param)
    {
        case DI_PARAM_NAME:
            copy_name_locked(rec.name, v);
            break;
        case DI_PARAM_ID:
            rec.id = (uint16_t)v->u;
            break;
        case DI_PARAM_DEBOUNCE_MS:
            rec.debounce_ms = (uint16_t)v->u;
            break;
        case DI_PARAM_POLARITY:
            rec.polarity = (di_polarity_t)v->u;
            break;
        case DI_PARAM_FAULT_STATE:
            rec.fault_state = (fault_state_t)v->u;
            break;
        case DI_PARAM_INTERRUPT_ENABLED:
            rec.interrupt_enabled = v->b;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_di(idx, &rec);
}

static config_status_t
apply_do (uint8_t idx, uint16_t param, const eeprom_value_t * v)
{
    do_config_t     rec;
    config_status_t st = config_get_do(idx, &rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((do_param_t)param)
    {
        case DO_PARAM_NAME:
            copy_name_locked(rec.name, v);
            break;
        case DO_PARAM_ID:
            rec.id = (uint16_t)v->u;
            break;
        case DO_PARAM_POLARITY:
            rec.polarity = (do_polarity_t)v->u;
            break;
        case DO_PARAM_FAULT_STATE:
            rec.fault_state = (fault_state_t)v->u;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_do(idx, &rec);
}

static config_status_t
apply_tc (uint8_t idx, uint16_t param, const eeprom_value_t * v)
{
    tc_config_t     rec;
    config_status_t st = config_get_tc(idx, &rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((tc_param_t)param)
    {
        case TC_PARAM_NAME:
            copy_name_locked(rec.name, v);
            break;
        case TC_PARAM_ID:
            rec.id = (uint16_t)v->u;
            break;
        case TC_PARAM_TC_TYPE:
            rec.tc_type = (tc_type_t)v->u;
            break;
        case TC_PARAM_UNIT:
            rec.unit = (tc_unit_t)v->u;
            break;
        case TC_PARAM_CJC_ENABLED:
            rec.cjc_enabled = v->b;
            break;
        case TC_PARAM_FILTER_MS:
            rec.filter_ms = (uint16_t)v->u;
            break;
        case TC_PARAM_FAULT_STATE:
            rec.fault_state = (fault_state_t)v->u;
            break;
        case TC_PARAM_FAULT_VALUE_C10:
            rec.fault_value_c10 = (int16_t)v->i;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_tc(idx, &rec);
}

static config_status_t
apply_ai (uint8_t idx, uint16_t param, const eeprom_value_t * v)
{
    ai_config_t     rec;
    config_status_t st = config_get_ai(idx, &rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((ai_param_t)param)
    {
        case AI_PARAM_NAME:
            copy_name_locked(rec.name, v);
            break;
        case AI_PARAM_ID:
            rec.id = (uint16_t)v->u;
            break;
        case AI_PARAM_INPUT_MODE:
            rec.input_mode = (ai_input_mode_t)v->u;
            break;
        case AI_PARAM_FILTER_MS:
            rec.filter_ms = (uint16_t)v->u;
            break;
        case AI_PARAM_SCALE_NUM:
            rec.scale_num = v->i;
            break;
        case AI_PARAM_SCALE_DEN:
            rec.scale_den = v->i;
            break;
        case AI_PARAM_OFFSET:
            rec.offset = v->i;
            break;
        case AI_PARAM_FAULT_STATE:
            rec.fault_state = (fault_state_t)v->u;
            break;
        case AI_PARAM_FAULT_VALUE:
            rec.fault_value = v->i;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_ai(idx, &rec);
}

static config_status_t
apply_ao (uint8_t idx, uint16_t param, const eeprom_value_t * v)
{
    ao_config_t     rec;
    config_status_t st = config_get_ao(idx, &rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((ao_param_t)param)
    {
        case AO_PARAM_NAME:
            copy_name_locked(rec.name, v);
            break;
        case AO_PARAM_ID:
            rec.id = (uint16_t)v->u;
            break;
        case AO_PARAM_OUTPUT_MODE:
            rec.output_mode = (ao_output_mode_t)v->u;
            break;
        case AO_PARAM_FAULT_STATE:
            rec.fault_state = (fault_state_t)v->u;
            break;
        case AO_PARAM_FAULT_VALUE:
            rec.fault_value = v->i;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_ao(idx, &rec);
}

static config_status_t
apply_pcnt (uint8_t idx, uint16_t param, const eeprom_value_t * v)
{
    pcnt_config_t   rec;
    config_status_t st = config_get_pcnt(idx, &rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((pcnt_param_t)param)
    {
        case PCNT_PARAM_NAME:
            copy_name_locked(rec.name, v);
            break;
        case PCNT_PARAM_ID:
            rec.id = (uint16_t)v->u;
            break;
        case PCNT_PARAM_MODE:
            rec.mode = (pcnt_mode_t)v->u;
            break;
        case PCNT_PARAM_EDGE:
            rec.edge = (pcnt_edge_t)v->u;
            break;
        case PCNT_PARAM_LIMIT:
            rec.limit = v->u;
            break;
        case PCNT_PARAM_RESET_ON_READ:
            rec.reset_on_read = v->b;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_pcnt(idx, &rec);
}

static config_status_t
apply_pwm (uint8_t idx, uint16_t param, const eeprom_value_t * v)
{
    pwm_config_t    rec;
    config_status_t st = config_get_pwm(idx, &rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((pwm_param_t)param)
    {
        case PWM_PARAM_NAME:
            copy_name_locked(rec.name, v);
            break;
        case PWM_PARAM_ID:
            rec.id = (uint16_t)v->u;
            break;
        case PWM_PARAM_PERIOD_US:
            rec.period_us = v->u;
            break;
        case PWM_PARAM_DUTY_PERMILLE:
            rec.duty_permille = (uint16_t)v->u;
            break;
        case PWM_PARAM_FAULT_STATE:
            rec.fault_state = (fault_state_t)v->u;
            break;
        case PWM_PARAM_FAULT_DUTY_PERMILLE:
            rec.fault_duty_permille = (uint16_t)v->u;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_pwm(idx, &rec);
}

static config_status_t
apply_system (uint16_t param, const eeprom_value_t * v)
{
    system_config_t rec;
    config_status_t st = config_get_system(&rec);
    if (st != CONFIG_OK)
    {
        return st;
    }
    switch ((system_param_t)param)
    {
        case SYSTEM_PARAM_CANOPEN_NODE_ID:
            rec.canopen_node_id = (uint8_t)v->u;
            break;
        case SYSTEM_PARAM_CAN_BITRATE:
            rec.can_bitrate = (can_bitrate_t)v->u;
            break;
        case SYSTEM_PARAM_HEARTBEAT_MS:
            rec.heartbeat_ms = (uint16_t)v->u;
            break;
        case SYSTEM_PARAM_SYNC_WINDOW_US:
            rec.sync_window_us = v->u;
            break;
        case SYSTEM_PARAM_NMT_STARTUP:
            rec.nmt_startup = (nmt_startup_t)v->u;
            break;
        case SYSTEM_PARAM_PRODUCER_EMCY_COB_ID:
            rec.producer_emcy_cob_id = v->u;
            break;
        default:
            return CONFIG_ERR_INVALID;
    }
    return config_set_system(&rec);
}

static config_status_t
apply_field (const eeprom_req_t * req)
{
    switch (req->type)
    {
        case EEPROM_TYPE_IO_DI:
            return apply_di(req->item, req->param, &req->value);
        case EEPROM_TYPE_IO_DO:
            return apply_do(req->item, req->param, &req->value);
        case EEPROM_TYPE_IO_TC:
            return apply_tc(req->item, req->param, &req->value);
        case EEPROM_TYPE_IO_AI:
            return apply_ai(req->item, req->param, &req->value);
        case EEPROM_TYPE_IO_AO:
            return apply_ao(req->item, req->param, &req->value);
        case EEPROM_TYPE_IO_PCNT:
            return apply_pcnt(req->item, req->param, &req->value);
        case EEPROM_TYPE_IO_PWM:
            return apply_pwm(req->item, req->param, &req->value);
        case EEPROM_TYPE_SYSTEM:
            return apply_system(req->param, &req->value);
        default:
            return CONFIG_ERR_INVALID;
    }
}

/* ====================================================================
 * Task body
 * ==================================================================== */

static bool
process_request (const eeprom_req_t * req)
{
    if (req->kind == REQ_KIND_COMMIT)
    {
        printf("[eeprom] commit request (monolithic)\n");
        return true; /* cache already mutated by caller; just save */
    }
    const config_status_t st = apply_field(req);
    if (st == CONFIG_OK)
    {
        printf("[eeprom] applied type=%u item=%u param=%u\n",
               (unsigned)req->type,
               (unsigned)req->item,
               (unsigned)req->param);
        return true;
    }
    printf("[eeprom] REJECT type=%u item=%u param=%u -> %s\n",
           (unsigned)req->type,
           (unsigned)req->item,
           (unsigned)req->param,
           config_print_status(st));
    return false; /* don't mark dirty for rejected fields */
}

static void
eeprom_task (void * pv)
{
    (void)pv;
    configASSERT(s_queue != NULL);

    for (;;)
    {
        eeprom_req_t req;
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        bool dirty = process_request(&req);

        /* Coalesce: drain anything queued while we were applying.
         * Stops at the first empty receive — newcomers after that
         * point will be picked up on the next outer iteration. */
        unsigned coalesced = 0;
        while (xQueueReceive(s_queue, &req, 0) == pdPASS)
        {
            dirty |= process_request(&req);
            coalesced++;
        }
        if (coalesced > 0)
        {
            printf("[eeprom] coalesced %u extra request(s) into this save\n",
                   coalesced);
        }

        if (dirty)
        {
            printf("[eeprom] config_save...\n");
            const config_status_t st = config_save();
            printf("[eeprom] config_save -> %s\n", config_print_status(st));
        }
        else
        {
            printf("[eeprom] nothing dirty; skipping save\n");
        }
    }
}

/* ====================================================================
 * Public API
 * ==================================================================== */

config_status_t
eeprom_manager_init (void)
{
    /* "Runs first on boot to load initial settings". Synchronous so
     * subsequent producers see a populated cache before they post.
     * storage_init must precede config_init — the latter immediately
     * reads slots through the storage driver. */
    const storage_status_t ss = storage_init();
    printf("[eeprom] storage_init -> %s\n", ss == STORAGE_OK ? "OK" : "ERR");
    if (ss != STORAGE_OK)
    {
        printf("[eeprom] continuing on defaults — slot reads will fail\n");
    }

    const config_status_t st = config_init();
    printf("[eeprom] config_init -> %s\n", config_print_status(st));

    if (s_queue == NULL)
    {
        s_queue = xQueueCreate(EEPROM_QUEUE_DEPTH, sizeof(eeprom_req_t));
        configASSERT(s_queue != NULL);

        const BaseType_t rc = xTaskCreate(eeprom_task,
                                          "eeprom-mgr",
                                          EEPROM_TASK_STACK,
                                          NULL,
                                          EEPROM_TASK_PRIORITY,
                                          &s_task);
        configASSERT(rc == pdPASS);
        (void)rc;
    }
    return st;
}

bool
QueueConfigChange (eeprom_config_type_t type,
                   uint8_t              item,
                   uint16_t             param,
                   eeprom_value_t       value)
{
    if (s_queue == NULL || type >= EEPROM_TYPE_COUNT)
    {
        return false;
    }
    const eeprom_req_t req = {
        .kind  = REQ_KIND_FIELD,
        .type  = type,
        .item  = item,
        .param = param,
        .value = value,
    };
    return xQueueSend(s_queue, &req, EEPROM_ENQ_TIMEOUT) == pdPASS;
}

bool
QueueConfigCommit (void)
{
    if (s_queue == NULL)
    {
        return false;
    }
    const eeprom_req_t req = { .kind = REQ_KIND_COMMIT };
    return xQueueSend(s_queue, &req, EEPROM_ENQ_TIMEOUT) == pdPASS;
}
