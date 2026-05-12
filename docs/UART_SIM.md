# Simulated UART over TCP (AT protocol)

The application exposes a tiny TCP-based stand-in for a UART link so you can
drive the configuration manager from your host while the FreeRTOS POSIX build
runs inside Docker. There is no real serial device involved — `socket(2)` on
the GCC_POSIX port replaces what would be a chip UART driver on a real target.

- Driver: [src/drivers/uart_sim.c](../src/drivers/uart_sim.c) / [src/drivers/uart_sim.h](../src/drivers/uart_sim.h)
- Wired up in [src/main.c](../src/main.c) via `uart_sim_init(5555)` before the scheduler starts.
- Container publishes the port via `EXPOSE 5555` in [Dockerfile](../Dockerfile).

## 1. Build & run the container

```bash
docker build -t config-wp .
docker run --rm -it -p 5555:5555 config-wp
```

The VS Code task **Run application** in [.vscode/tasks.json](../.vscode/tasks.json) already passes `-p 5555:5555`.

You should see:

```
[uart-sim] listening on 0.0.0.0:5555 (TCP, AT protocol)
```

## 2. Wire protocol

One TCP connection = one AT command frame. The driver reads until peer-EOF
(half-close), parses the first line as the command, then dispatches.

| Command | Reply | Side effect |
|---|---|---|
| `AT` | `OK\n` | none — liveness ping |
| `AT+GET_CONFIG` | `<JSON>\nOK\n` (or `ERROR: ...\n`) | reads cache via `config_export_json` |
| `AT+SET_CONFIG\n<JSON>` | `OK accepted=N rejected=N unknown_keys=N\n` (or `ERROR: ...\n`) | `config_import_json` + `config_save` (commit to EEPROM/flash) |
| anything else | `ERROR: unknown command\n` | none |

Notes:

- Command tokens are case-insensitive (`AT+get_config` works).
- `AT+SET_CONFIG` expects the JSON payload to start on the **next line** after
  the command. Everything from the first `\n` (or `\r\n`) to peer-EOF is
  treated as the payload.
- Both buffers (RX and TX) are 16 KiB. Larger configs need bigger statics in
  [src/drivers/uart_sim.c](../src/drivers/uart_sim.c).
- On `AT+SET_CONFIG`, the import and save run synchronously on the listener
  task, so the reply genuinely confirms the bytes hit storage.

## 3. Driving it from the host (macOS)

macOS ships BSD `nc`, which has neither `-q` nor a usable `-N`. The patterns
below avoid both flags.

### 3a. AT+GET_CONFIG

```bash
printf 'AT+GET_CONFIG\n' | nc localhost 5555
```

Pipe through `jq` for pretty output (drop the trailing `OK` line):

```bash
printf 'AT+GET_CONFIG\n' | nc localhost 5555 | sed '$d' | jq .
```

### 3b. AT+SET_CONFIG

The command line is followed by the JSON document — concatenate them:

```bash
( printf 'AT+SET_CONFIG\n'; cat examples/config.json; sleep 0.3 ) \
    | nc localhost 5555
```

Expected reply:

```
OK accepted=8 rejected=0 unknown_keys=7
```

If the JSON is malformed or fields are invalid you get something like:

```
ERROR: import failed (ERR_INVALID): di[3].debounce_ms out of range
```

Python alternative (no extra tools, clean half-close, exits on its own):

```bash
python3 - <<'PY'
import socket
s = socket.create_connection(("localhost", 5555))
s.sendall(b"AT+SET_CONFIG\n")
with open("examples/config.json", "rb") as f:
    s.sendall(f.read())
s.shutdown(socket.SHUT_WR)
print(s.recv(4096).decode(), end="")
PY
```

### 3c. AT (liveness)

```bash
printf 'AT\n' | nc localhost 5555
# -> OK
```

## 4. What you'll see in the container

```
[uart-sim] rx 14 bytes from 192.168.65.1:56855
[uart-sim] AT+GET_CONFIG -> OK (8836 bytes)

[uart-sim] rx 2473 bytes from 192.168.65.1:16497
[uart-sim] AT+SET_CONFIG payload 2459 bytes — importing...
[uart-sim] config_import_json -> OK (accepted=8 rejected=0 unknown_keys=7 malformed=0)
[uart-sim] persisting cache to storage...
[uart-sim] config_save -> OK
```

## 5. Threading & ownership notes

- `uart_sim_task` runs at `tskIDLE_PRIORITY + 1`. The POSIX port implements
  blocking `accept`/`recv` by parking the underlying pthread, so other
  FreeRTOS tasks keep running.
- `config_import_json`, `config_export_json`, and `config_save` all take the
  config manager's internal mutex, so it is safe to call them directly from
  the listener task while the app task is also touching the cache.
- One connection at a time (`listen(fd, 1)` + serial accept loop). The single
  shared `s_rx` / `s_tx` buffers are therefore conflict-free.
- The legacy async `external_comms_submit` path remains available for any
  caller that wants fire-and-forget queued imports — see
  [src/external_comms.h](../src/external_comms.h). The AT driver does not use
  it because `AT+SET_CONFIG` needs to ack the client after persistence.

## 6. Limitations

- No framing on a long-lived connection — each AT command is its own
  connection. Add CRLF framing in `uart_sim_task` if you need a persistent
  session.
- Plain-text protocol, no auth. This is a development tool.
- Host-only. On real hardware, replace this module with a chip UART driver
  that feeds the same `config_import_json` / `config_export_json` API;
  nothing above the driver layer changes.
