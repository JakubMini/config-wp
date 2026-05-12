#include "config_print.h"

#include <stdio.h>

/* ===================================================================
 * Enum -> string helpers
 * =================================================================== */

const char *
config_print_status (config_status_t s)
{
    switch (s)
    {
        case CONFIG_OK:
            return "OK";
        case CONFIG_ERR_NOT_INITIALISED:
            return "ERR_NOT_INITIALISED";
        case CONFIG_ERR_INDEX:
            return "ERR_INDEX";
        case CONFIG_ERR_INVALID:
            return "ERR_INVALID";
        case CONFIG_ERR_STORAGE:
            return "ERR_STORAGE";
        case CONFIG_ERR_CODEC:
            return "ERR_CODEC";
        case CONFIG_ERR_TOO_LARGE:
            return "ERR_TOO_LARGE";
        default:
            return "?";
    }
}

static const char *
prvBitrateStr (can_bitrate_t b)
{
    switch (b)
    {
        case CAN_BITRATE_125K:
            return "125 kbit/s";
        case CAN_BITRATE_250K:
            return "250 kbit/s";
        case CAN_BITRATE_500K:
            return "500 kbit/s";
        case CAN_BITRATE_1M:
            return "1 Mbit/s";
        default:
            return "?";
    }
}

static const char *
prvNmtStartupStr (nmt_startup_t n)
{
    switch (n)
    {
        case NMT_STARTUP_WAIT:
            return "WAIT (operator triggers NMT)";
        case NMT_STARTUP_AUTOSTART:
            return "AUTOSTART (Operational on boot)";
        default:
            return "?";
    }
}

static const char *
prvDiPolarityStr (di_polarity_t p)
{
    switch (p)
    {
        case DI_POLARITY_ACTIVE_HIGH:
            return "ACTIVE_HIGH";
        case DI_POLARITY_ACTIVE_LOW:
            return "ACTIVE_LOW";
        default:
            return "?";
    }
}

static const char *
prvFaultStateStr (fault_state_t f)
{
    switch (f)
    {
        case FAULT_STATE_HOLD:
            return "HOLD (keep last good)";
        case FAULT_STATE_LOW:
            return "LOW (force 0 / off)";
        case FAULT_STATE_HIGH:
            return "HIGH (force 1 / on)";
        default:
            return "?";
    }
}

static const char *
prvBoolStr (bool b)
{
    return b ? "true" : "false";
}

/* ===================================================================
 * Record dumps
 * =================================================================== */

void
config_print_system (const char * label)
{
    system_config_t sys;
    config_status_t st = config_get_system(&sys);
    if (st != CONFIG_OK)
    {
        printf("[cfg] get_system (%s) -> %s\n", label, config_print_status(st));
        return;
    }

    /* OD 0x1014 sentinel: if producer_emcy_cob_id == 0, the runtime
     * CANopen stack derives 0x80 + node_id at NMT-Operational entry.
     * Print BOTH the stored value and the effective COB-ID so the
     * operator sees what's actually on the wire. */
    const uint32_t emcy_stored = sys.producer_emcy_cob_id;
    const uint32_t emcy_effective
        = (emcy_stored == 0u) ? (0x80u + (uint32_t)sys.canopen_node_id)
                              : emcy_stored;
    const char * emcy_note = (emcy_stored == 0u)
                                 ? "sentinel; derived 0x80 + node_id"
                                 : "operator override";

    printf("\n[cfg] ===== system (%s) =====\n", label);
    printf("[cfg]   %-23s: %u\n",
           "canopen_node_id",
           (unsigned)sys.canopen_node_id);
    printf(
        "[cfg]   %-23s: %s\n", "can_bitrate", prvBitrateStr(sys.can_bitrate));
    printf("[cfg]   %-23s: %u\n",
           "heartbeat_ms (0x1017)",
           (unsigned)sys.heartbeat_ms);
    printf("[cfg]   %-23s: %u\n",
           "sync_window_us (0x1007)",
           (unsigned)sys.sync_window_us);
    printf("[cfg]   %-23s: %s\n",
           "nmt_startup",
           prvNmtStartupStr(sys.nmt_startup));
    printf("[cfg]   %-23s: stored=0x%08x  effective=0x%03x  (%s)\n",
           "emcy_cob_id (0x1014)",
           (unsigned)emcy_stored,
           (unsigned)emcy_effective,
           emcy_note);
}

void
config_print_di (uint8_t idx)
{
    di_config_t     di;
    config_status_t st = config_get_di(idx, &di);
    if (st != CONFIG_OK)
    {
        printf("[cfg] get_di(%u) -> %s\n", idx, config_print_status(st));
        return;
    }

    printf("\n[cfg] ===== di[%u] =====\n", idx);
    printf("[cfg]   %-23s: \"%s\"\n", "name", di.name);
    printf("[cfg]   %-23s: 0x%04x\n", "id", (unsigned)di.id);
    printf("[cfg]   %-23s: %u\n", "debounce_ms", (unsigned)di.debounce_ms);
    printf("[cfg]   %-23s: %s\n", "polarity", prvDiPolarityStr(di.polarity));
    printf(
        "[cfg]   %-23s: %s\n", "fault_state", prvFaultStateStr(di.fault_state));
    printf("[cfg]   %-23s: %s\n",
           "interrupt_enabled",
           prvBoolStr(di.interrupt_enabled));
}

void
config_print_stage (const char * title)
{
    printf(
        "\n[app] "
        "============================================================\n");
    printf("[app]  %s\n", title);
    printf(
        "[app] "
        "============================================================\n");
}
