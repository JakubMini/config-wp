/*****************************************************************************
 * Module:  uart_sim
 * Purpose: TCP-backed UART simulator for the POSIX FreeRTOS port. See
 *          header for the wire contract and host-side usage.
 *
 *          Speaks a small AT-style line protocol. One TCP connection
 *          carries one command frame. Recognised commands:
 *
 *              AT
 *                  Liveness ping. Replies "OK\n".
 *
 *              AT+GET_CONFIG
 *                  Reads the cache via config_export_json and writes
 *                  the JSON document followed by "\nOK\n".
 *
 *              AT+SET_CONFIG\n<JSON...>
 *                  Everything after the first newline is treated as a
 *                  JSON patch. The driver calls config_import_json and,
 *                  on success, calls config_queue_change() to ask the
 *                  EEPROM Manager (the registered change-hook owner)
 *                  to commit. Persistence runs out-of-band; the reply
 *                  reports import counts and whether a consumer was
 *                  registered to receive the change notification.
 *
 *          Unknown commands get "ERROR: unknown command\n".
 *****************************************************************************/

#include "uart_sim.h"

#include "FreeRTOS.h"
#include "task.h"

#include "application/config.h"
#include "application/config_json.h"
#include "application/eeprom_manager.h"
#include "config_print.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UART_SIM_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define UART_SIM_TASK_STACK    (configMINIMAL_STACK_SIZE * 8U)
#define UART_SIM_RX_BUF_BYTES  16384U
#define UART_SIM_TX_BUF_BYTES  16384U

static char     s_rx[UART_SIM_RX_BUF_BYTES];
static char     s_tx[UART_SIM_TX_BUF_BYTES];
static uint16_t s_port = 0U;

/* Best-effort full send. Returns false on hard error. */
static bool
uart_sim_send_all (int fd, const char * data, size_t len)
{
    size_t off = 0U;
    while (off < len)
    {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        return false;
    }
    return true;
}

static void
uart_sim_send_line (int fd, const char * s)
{
    (void)uart_sim_send_all(fd, s, strlen(s));
}

/* Trim leading whitespace and uppercase a copy of [start, start+len) for
 * keyword comparison. Returns the number of bytes consumed up to (and
 * including) the first newline, so the caller knows where the payload
 * (if any) starts. */
static size_t
uart_sim_extract_cmd (const char * buf, size_t len, char * out, size_t out_cap)
{
    size_t i = 0U;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
    {
        i++;
    }
    size_t o = 0U;
    while (i < len && buf[i] != '\n' && buf[i] != '\r')
    {
        if (o + 1U < out_cap)
        {
            out[o++] = (char)toupper((unsigned char)buf[i]);
        }
        i++;
    }
    out[o] = '\0';
    /* Strip trailing whitespace from the command line. */
    while (o > 0U && (out[o - 1U] == ' ' || out[o - 1U] == '\t'))
    {
        out[--o] = '\0';
    }
    /* Skip the line terminator (\n, \r, or \r\n). */
    if (i < len && buf[i] == '\r')
    {
        i++;
    }
    if (i < len && buf[i] == '\n')
    {
        i++;
    }
    return i;
}

static void
uart_sim_handle_get (int cfd)
{
    size_t          written = 0U;
    config_status_t st      = config_export_json(s_tx, sizeof(s_tx), &written);
    printf("[uart-sim] AT+GET_CONFIG -> %s (%zu bytes)\n",
           config_print_status(st),
           written);
    if (st != CONFIG_OK)
    {
        char err[96];
        int  n = snprintf(err,
                         sizeof(err),
                         "ERROR: export failed (%s)\n",
                         config_print_status(st));
        if (n > 0)
        {
            (void)uart_sim_send_all(cfd, err, (size_t)n);
        }
        return;
    }
    (void)uart_sim_send_all(cfd, s_tx, written);
    uart_sim_send_line(cfd, "\nOK\n");
}

static void
uart_sim_handle_set (int cfd, const char * payload, size_t payload_len)
{
    if (payload == NULL || payload_len == 0U)
    {
        printf("[uart-sim] AT+SET_CONFIG -> no payload\n");
        uart_sim_send_line(cfd, "ERROR: missing JSON payload\n");
        return;
    }

    printf("[uart-sim] AT+SET_CONFIG payload %zu bytes — importing...\n",
           payload_len);

    config_import_report_t rep = { 0 };
    config_status_t        ist = config_import_json(payload, payload_len, &rep);
    printf(
        "[uart-sim] config_import_json -> %s "
        "(accepted=%u rejected=%u unknown_keys=%u malformed=%u)\n",
        config_print_status(ist),
        (unsigned)rep.accepted,
        (unsigned)rep.rejected,
        (unsigned)rep.unknown_keys,
        (unsigned)rep.malformed);

    if (ist != CONFIG_OK)
    {
        char err[160];
        int  n = snprintf(err,
                         sizeof(err),
                         "ERROR: import failed (%s)%s%s\n",
                         config_print_status(ist),
                         rep.first_error[0] != '\0' ? ": " : "",
                         rep.first_error[0] != '\0' ? rep.first_error : "");
        if (n > 0)
        {
            (void)uart_sim_send_all(cfd, err, (size_t)n);
        }
        return;
    }

    printf("[uart-sim] queueing commit to EEPROM Manager...\n");
    const bool queued = QueueConfigCommit();
    printf("[uart-sim] QueueConfigCommit -> %s\n", queued ? "queued" : "FAIL");

    char ok[192];
    int  n = snprintf(ok,
                     sizeof(ok),
                     "OK accepted=%u rejected=%u unknown_keys=%u "
                      "persist=%s\n",
                     (unsigned)rep.accepted,
                     (unsigned)rep.rejected,
                     (unsigned)rep.unknown_keys,
                     queued ? "queued" : "FAIL");
    if (n > 0)
    {
        (void)uart_sim_send_all(cfd, ok, (size_t)n);
    }
}

static int
uart_sim_listen (uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        printf("[uart-sim] socket() failed: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("[uart-sim] bind(:%u) failed: %s\n",
               (unsigned)port,
               strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0)
    {
        printf("[uart-sim] listen() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("[uart-sim] listening on 0.0.0.0:%u (TCP, AT protocol)\n",
           (unsigned)port);
    return fd;
}

static void
uart_sim_task (void * pv)
{
    (void)pv;

    int listen_fd = uart_sim_listen(s_port);
    if (listen_fd < 0)
    {
        vTaskDelete(NULL);
        return;
    }

    for (;;)
    {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        int cfd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            printf("[uart-sim] accept() failed: %s\n", strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t off = 0U;
        size_t cap = sizeof(s_rx) - 1U; /* room for NUL */
        for (;;)
        {
            ssize_t n = recv(cfd, s_rx + off, cap - off, 0);
            if (n > 0)
            {
                off += (size_t)n;
                if (off >= cap)
                {
                    printf("[uart-sim] rx truncated at %zu bytes\n", off);
                    break;
                }
                continue;
            }
            if (n == 0)
            {
                break; /* peer closed -> end-of-frame */
            }
            if (errno == EINTR)
            {
                continue;
            }
            printf("[uart-sim] recv() failed: %s\n", strerror(errno));
            break;
        }
        s_rx[off] = '\0';

        printf("[uart-sim] rx %zu bytes from %s:%u\n",
               off,
               inet_ntoa(peer.sin_addr),
               (unsigned)ntohs(peer.sin_port));

        if (off == 0U)
        {
            close(cfd);
            continue;
        }

        char         cmd[64];
        const size_t hdr_len
            = uart_sim_extract_cmd(s_rx, off, cmd, sizeof(cmd));

        if (strcmp(cmd, "AT+GET_CONFIG") == 0)
        {
            uart_sim_handle_get(cfd);
        }
        else if (strcmp(cmd, "AT+SET_CONFIG") == 0)
        {
            const char * payload     = s_rx + hdr_len;
            const size_t payload_len = off - hdr_len;
            uart_sim_handle_set(cfd, payload, payload_len);
        }
        else if (strcmp(cmd, "AT") == 0)
        {
            printf("[uart-sim] AT -> OK\n");
            uart_sim_send_line(cfd, "OK\n");
        }
        else
        {
            printf("[uart-sim] unknown command: \"%s\"\n", cmd);
            uart_sim_send_line(cfd, "ERROR: unknown command\n");
        }

        close(cfd);
    }
}

void
uart_sim_init (uint16_t port)
{
    s_port        = port;
    BaseType_t rc = xTaskCreate(uart_sim_task,
                                "uart-sim",
                                UART_SIM_TASK_STACK,
                                NULL,
                                UART_SIM_TASK_PRIORITY,
                                NULL);
    configASSERT(rc == pdPASS);
    (void)rc;
}
