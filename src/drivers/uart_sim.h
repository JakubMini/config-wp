/*****************************************************************************
 * Module:  uart_sim
 * Purpose: Host-only stand-in for a UART RX path. Listens on a TCP port
 *          inside the container; each accepted connection is treated as
 *          one "frame": bytes are read until EOF, then handed to the
 *          external_comms consumer task verbatim.
 *
 *          From the host:
 *              nc -q1 localhost 5555 < examples/config.json
 *
 *          The driver owns a small ring of static RX buffers so the
 *          {pointer,len} message handed to external_comms remains valid
 *          until the consumer drains it. One connection at a time.
 *****************************************************************************/

#ifndef DRIVERS_UART_SIM_H
#define DRIVERS_UART_SIM_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

/* Spawn the listener task. Call before vTaskStartScheduler.
 * port: TCP port to bind on 0.0.0.0 (e.g. 5555). */
void uart_sim_init (uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_UART_SIM_H */
