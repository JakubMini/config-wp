/*****************************************************************************
 * Module:  external_comms
 * Purpose: Dedicated FreeRTOS task that receives JSON configuration
 *          patches (typically arriving over CAN / UART / CLI in a real
 *          device) and applies them via config_import_json().
 *
 *          Architectural intent: keep the heap-using JSON parse and the
 *          potentially long import work OFF the IO hot path. Producers
 *          push {pointer, length} pairs through a queue; the task drains
 *          the queue and feeds each blob through the manager's
 *          validating setters.
 *
 *          Threading:
 *            - external_comms_init() is NOT thread-safe; call once
 *              before vTaskStartScheduler (or from a single setup
 *              context).
 *            - external_comms_submit() IS thread-safe — multiple
 *              producers can post concurrently; the queue serialises.
 *
 *          Buffer ownership: the JSON buffer must remain valid until
 *          the consumer task has processed the message. Long-lived
 *          producers (CAN/UART RX rings, static demo literals) satisfy
 *          this trivially. Stack/heap producers should wait for the
 *          consumer to drain before reusing — currently observed via
 *          task delay; a done-semaphore handshake can be retrofitted.
 *
 *          Persistence: this layer does NOT call config_save(). The
 *          cache changes are in-RAM only after submit. Callers commit
 *          to flash separately when they want — typically one save
 *          after a series of patches, not one per patch.
 *****************************************************************************/

#ifndef EXTERNAL_COMMS_H
#define EXTERNAL_COMMS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stddef.h>

/* Create the comms queue and spawn the consumer task. Idempotent on
 * repeat calls. Must be invoked before any external_comms_submit. */
void external_comms_init (void);

/* Hand a JSON blob to the consumer task. Returns true on successful
 * enqueue, false if the queue is full or arguments are invalid. The
 * buffer at `json` must remain valid until the consumer processes it
 * (see header comment for ownership rules). */
bool external_comms_submit (const char * json, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EXTERNAL_COMMS_H */
