# Design

Design decisions baked into the configuration manager. Sections land as
each layer goes in.

---

## Data model

Single source of truth for what configuration looks like in RAM: one
struct per IO channel type (DI, DO, TC, AI, AO, PCNT, PWM), one for
system config, typed enums for constrained fields, and a factory-default
table per type.

```
src/application/
  config_limits.h         channel counts, name length
  config_types.h          enums + structs + io_domain_t
  config_defaults.{h,c}   extern decls + static const tables
```

### Channel counts (`config_limits.h`)

Compile-time `#define`s drawn from the hardware diagram:

| Type | Count | Source |
| ---- | ----: | ------ |
| DI   | 16 | 2 × PCA9555 |
| DO   | 16 | 2 × PCA9555 |
| TC   |  4 | MAX31856 array |
| AI   |  8 | ADC1.IN1..IN8 |
| AO   |  4 | DAC (placeholder) |
| PCNT |  4 | TIM2.CH1..CH4 |
| PWM  |  4 | TIMn.CH1..CH4 |

`CONFIG_NAME_LEN = 16` (15 chars + NUL). Matches CiA 401 device-info
string conventions and fits one TLV record. Defaults tables use array
range-init so bumping a count scales without per-index edits.

### Per-type fields (host sizeof pinned by `tests/test_types.cpp`)

| Type | Fields | sizeof |
| ---- | ------ | ----: |
| `di_config_t`     | name, id, debounce_ms, polarity, fault_state, interrupt_enabled | 32 |
| `do_config_t`     | name, id, polarity, fault_state | 28 |
| `tc_config_t`     | name, id, tc_type, unit, cjc_enabled, filter_ms, fault_state, fault_value_c10 | 40 |
| `ai_config_t`     | name, id, input_mode, filter_ms, scale_num, scale_den, offset, fault_state, fault_value | 48 |
| `ao_config_t`     | name, id, output_mode, slew_per_s, scale_num, scale_den, offset, fault_state, fault_value | 48 |
| `pcnt_config_t`   | name, id, mode, edge, limit, reset_on_read | 36 |
| `pwm_config_t`    | name, id, period_us, duty_permille, fault_state, fault_duty_permille | 36 |
| `system_config_t` | canopen_node_id, can_bitrate, heartbeat_ms, sync_window_us, nmt_startup, producer_emcy_cob_id | 20 |

### Notable field choices

- **`char name[16]`** — fixed-size, no allocator, maps directly to a TLV
  record. Over-long names rejected at the setter boundary.
- **`uint16_t id`** — mirrors CANopen OD index width; one record = one
  `(domain, index)` pair.
- **Integer AI/AO scaling** — `value = (raw * num) / den + offset`
  stored as three `int32_t`. No FPU dependency, deterministic across
  toolchains, maps onto CiA 401 PV factor/offset (0x6126/0x6127).
- **`fault_state_t`** — `HOLD=0`, `LOW=1`, `HIGH=2`. `HOLD` is zero so
  zero-initialised records land in the safe state (pinned by
  `Types.FaultStateHoldIsZero`). Analog types (AI/AO/TC) carry an extra
  `fault_value` field applied when `fault_state != HOLD`.
- **Enums with `_COUNT` sentinel** — range checks read as
  `x.tc_type < TC_TYPE_COUNT` without hard-coding the last legal value.
- **`producer_emcy_cob_id == 0` is a sentinel** — derive `0x80 + node_id`
  at NMT startup; non-zero is an operator override used verbatim. Avoids
  silent staleness when `canopen_node_id` changes later. Same pattern
  generalises to predefined PDO COB-IDs.
- **`io_domain_t`** — stable top-level enum (`DI=0..PWM=6`); append-only
  so older firmware can read newer configs.

### Factory defaults (`config_defaults.{h,c}`)

One `const` table per IO type plus `g_system_defaults`, `extern const`
in the header so every TU sees the same definition and tables stay in
flash. Populated via the gcc/clang range-init extension:

```c
const di_config_t g_di_defaults[CONFIG_NUM_DI] = {
    [0 ... CONFIG_NUM_DI - 1] = { .debounce_ms = 10, ... },
};
```

Not strict ISO C. Suppressed *locally* in `config_defaults.c` only
(`-Wgnu-designator`, `-Wpedantic`) so the rest of the codebase keeps
strict-warning enforcement. Alternatives rejected: per-index spelling
(error-prone — silent zero-fill on forgotten indices); runtime
`config_defaults_init()` (loses `static const` flash placement).

Defaults are exercised on (1) fresh device — both slots blank/corrupt,
(2) explicit `config_reset_defaults()`, (3) per-field when a loaded
record predates a new field. Values are conservative: empty names, ID 0,
`HOLD` for inputs, `LOW` for outputs, 100 ms TC filter, passthrough
scaling, 100 Hz PWM @ 0% duty, CAN 500 kbit/s, node 1, NMT
wait-for-command.

### Modularity

- **More channels** — bump `CONFIG_NUM_*` and rebuild.
- **New IO type** — touch four places: struct in `config_types.h`,
  count in `config_limits.h`, defaults in `config_defaults.c`,
  `IO_DOMAIN_COUNT` in the test. TLV codec and JSON emitter add two
  more once they exist.
- **Board variants** — deferred; a board-variant header overriding
  `CONFIG_NUM_*` is the planned path.

### Non-goals at this layer

No allocation, no I/O, no API, no threading, no persistence. No
CAN-specific wire encoding — `system_config_t` describes *what* the
device should do with CAN, not the OD layout. No active fault detection
— fault-state fields describe *behaviour when a fault is signalled*.

---

## Storage layer

```
src/application/
  crc32.{h,c}         CRC-32/ISO-HDLC, one-shot + streaming API
  config_slot.{h,c}   A/B-slot persistence protocol
```

The slot layer is **type-agnostic** — opaque byte payloads, no
dependency on `config_types.h`. The TLV codec sits between typed config
and this byte-payload interface.

### On-flash layout

```
+-------------------+ offset 0
| Slot A header     | 20 bytes
| Slot A payload    | up to SLOT_PAYLOAD_MAX
+-------------------+ offset SLOT_TOTAL
| Slot B header     |
| Slot B payload    |
+-------------------+
```

Header (`slot_header_t`, packed to 20 bytes, `static_assert`'d):
`magic` (`0xC0FC0FCA`), `format_ver` (=1), `flags` (reserved=0), `seq`
(monotonic per-write counter), `length`, `crc32`. CRC covers
`header[0..crc32) + payload`; CRC field last so it's computed in one
streaming pass.

### Header sanity (before CRC, no payload read)

`slot_header_looks_sane` rejects when: wrong `magic`, wrong
`format_ver` (forces clean break on layout changes — old firmware
refuses new slots and vice versa), non-zero `flags` (reserved for
future use like signed slots), or `length > SLOT_PAYLOAD_MAX`. Cheap
short-circuit on blank/unfamiliar slots. CRC is the final gate.

### Protocol

**Boot — `slot_pick_active(out_id, buf, cap, out_len)`**
1. Read each header.
2. For each sane header, read the payload into `buf` and verify CRC.
3. Both valid → higher `seq` wins. One valid → that one. Neither →
   `SLOT_ERR_NO_VALID` (caller loads defaults).
4. Re-read winner's payload at the end (`buf` is scratch during
   validation).

**Write — `slot_write(payload, len)`**
1. Cheap header scan (no CRC, no payload read). Target the inactive
   slot. `seq = max(sane_seqs) + 1`, starting at 1.
2. Build header in RAM, compute CRC.
3. **Write payload first, then header.** The header is the commit
   record: a torn payload write leaves the slot rejectable on next
   boot via CRC failure; the other slot survives.

### Properties

- **Power-loss safe.** At most one slot corrupted; the other survives
  and is detected via CRC.
- **Bit-flip detection.** Cell flips and torn writes both manifest as
  CRC mismatches (false-negative ~2.3 × 10⁻¹⁰ per record).
- **Newer-wins.** `seq` (`u32`) resolves the both-valid case;
  ~130 years at 1 write/s before wrap.
- **No config-type dependency.** Reusable for any opaque payload.
- **No dynamic allocation.** All RAM caller-provided or stack;
  `crc32` LUT (1 KB) is a single module-local array.
- **Distinguishable errors.** `storage_read` failure returns
  `SLOT_ERR_STORAGE`, not `SLOT_ERR_NO_VALID` — caller separates
  "blank EEPROM" from "broken hardware".

### CRC init contract

`crc32_init()` MUST be called once at single-threaded startup before
any `crc32_compute`/`crc32_step`. No lazy self-init: a racy build of
the 1 KB LUT in a multi-task system is unsafe even though the final
values are deterministic. After init the table is read-only and
concurrent-safe. Debug builds assert on missed init. Test fixtures
call it in `SetUp`; `config_init()` will call it before any task
spawns.

### Out of scope

- **Crypto integrity.** CRC catches accidents, not tampering. If
  signed config is needed later, swap CRC for HMAC-SHA256; protocol
  otherwise unchanged.
- **Sub-record atomicity.** A 2 KB payload spans multiple EEPROM
  pages; torn writes can mix old/new bytes. CRC catches this;
  recovery is "fall back to the other slot," not stitch-together.

---

## TLV codec

Adapter between typed config structs and the opaque byte payload the
slot layer shuttles.

```
src/application/
  tlv.{h,c}           generic Tag-Length-Value iterator + writer
  config_codec.{h,c}  type-aware encoders/decoders + tag conventions
```

`tlv.{h,c}` is type-agnostic; `config_codec.{h,c}` defines how each
struct serialises and which tag identifies each record.

### Record format

```
+--------+--------+----------------------+
|  tag   | length | value (length bytes) |
+--------+--------+----------------------+
 2 bytes  2 bytes    0 .. 65535 bytes
```

Little-endian on the wire. Stream = concatenation of records, no
padding. u16 length caps a record at 64 KiB (largest is 38 bytes;
whole blob < 2 KB).

### Tag encoding

```
bits 15..8   domain   record type
bits  7..0   index    channel within the domain
```

`domain` mirrors `io_domain_t` for IO types; `0xF0` for the singleton
system record. Adding a new IO type adds one `io_domain_t` entry and a
new tag range — existing tags don't move, older firmware skips
unrecognised domains cleanly. Tag space = 65536 records.

### Wire sizes (value bytes; pinned by tests)

| record | bytes | record | bytes |
| --- | ---: | --- | ---: |
| DI | 23 | AO   | 38 |
| DO | 20 | PCNT | 25 |
| TC | 26 | PWM  | 27 |
| AI | 38 | System | 9 |

Wire size is **deliberately decoupled** from `sizeof(struct)`: the
encoder writes field-by-field with explicit widths, so struct padding
and host endianness don't leak into the on-flash representation.
Identical bytes on macOS clang, ARM gcc, or any other toolchain.

### Forward compatibility

1. **Skip unknown records.** Decoder uses `length` to advance past the
   value and continues.
2. **Preserve unknown records on rewrite.** `tlv_writer_emit_raw`
   appends a pre-encoded record verbatim. Higher layers capture
   unknowns on decode and re-emit on encode — byte-for-byte
   round-trip, no silent destruction of config data. Pinned by
   `ConfigCodecForwardCompat.UnknownRecordSurvivesRewrite`.

### Field-level limitation

TLV gives **record-level** forward compat only. Adding a field to
`di_config_t`:

- New firmware reading old records → short bytes → `TLV_ERR_TRUNCATED`
  → manager applies defaults for the record.
- Old firmware reading new records → trailing bytes ignored → new
  fields lost on re-save.

Field-level compat would require nested TLV-within-record. Deferred —
record-level suffices for the common case (adding IO types).

### Why TLV, not Protobuf

| concern | TLV | Protobuf |
| --- | --- | --- |
| Code size | ~200 lines C, no deps | nanopb +10–20 KB ROM + generated code |
| Runtime | one pass/record, u16 + memcpy | varint on every field, more general |
| Build | one `.c`/`.h` | `protoc` toolchain, two sources of truth |
| Field-level fwd compat | no (record-level only) | yes |
| CANopen OD alignment | `(domain, index)` ↔ OD `(index, subindex)` 1:1 | flat field numbers; OD side-table needed |
| Audit posture | every byte traceable to a line | generated code harder to audit |

**TLV is the minimum format that satisfies the README's forward-compat
and piecewise-update requirements.** Protobuf would too, at 10–100×
the code cost. Other formats considered and rejected: CBOR/MessagePack
(too general for a fixed schema), memcpy (no forward compat), DCF text
(blows the EEPROM budget — handled instead by the JSON layer above).

### Test coverage

`tests/test_tlv.cpp` (11 tests): well-formed bytes, sequential append,
`NO_SPACE`/`BUF`/`TRUNCATED`/`OVERRUN` errors, zero-length records,
iterator round-trip, empty buffer returns `TLV_END`, `emit_raw`
round-trip and validation.

`tests/test_config_codec.cpp` (13 tests): wire-size constants per type,
tag round-trip, non-default round-trip per type (8), truncated decode
rejection, name null-termination, unknown record survives a
decode/re-encode round-trip alongside known records.

---

## Extending the schema

The schema is the C code — no `.proto` or YAML. Tests pin every
consistency invariant; failing tests tell you what to update next.

### Adding a field to an existing record

| step | file | change |
| ---- | ---- | --- |
| 1 | `config_types.h` | add struct field |
| 2 | `config_defaults.c` | add default value |
| 3 | `tests/test_types.cpp` | bump sizeof bound if it grew |
| 4 | `config_codec.h` | bump `CONFIG_CODEC_*_WIRE_SIZE` |
| 5 | `config_codec.c` | add `put_*`/`get_*` calls |
| 6 | `tests/test_config_codec.cpp` | exercise new field in round-trip |
| 7 | `docs/DESIGN.md` | update tables |

Steps 4–5 must agree; encoder `assert` fires at runtime and
`ConfigCodecSizes.MatchDeclaredConstants` catches it in test.

### Adding a new IO type (e.g. RTD)

All of the above, plus: `CONFIG_NUM_RTD` in `config_limits.h`; append
`IO_DOMAIN_RTD` to `io_domain_t` (append-only — existing values stay
stable); bump `IO_DOMAIN_COUNT` in `test_types.cpp`; define
`CONFIG_CODEC_DOMAIN_RTD` and wire size in `config_codec.h`; (future)
JSON emitter/parser. Cost asymmetry — 6 files for a field vs 12 for a
type — is the explicit reason cross-language tooling would push us
toward a generator.

### Drift-prevention tests

| invariant | test |
| --- | --- |
| Wire size matches header constant | `ConfigCodecSizes.MatchDeclaredConstants` |
| Encode/decode agree on order/width | `ConfigCodecRoundTrip.*` per type |
| Struct didn't silently bloat | `Types.StructSizesWithinBudget` |
| Defaults in-range | `Defaults.{Di,Do,...}TableIsSane` |
| Enum size stable under `-fshort-enums` | `static_assert` per enum |
| `IO_DOMAIN_COUNT` matches type count | `Types.IoDomainMatchesChannelTypeCount` |
| `FAULT_STATE_HOLD == 0` | `Types.FaultStateHoldIsZero` |
| Unknown records survive round-trip | `ConfigCodecForwardCompat.UnknownRecordSurvivesRewrite` |

The moment a "tests pass but field X is wrong on the wire" failure
appears, we've lost the no-drift guarantee — that's the trigger for
moving to automation.

---

## API and threading

_(to follow as the manager API goes in.)_

## Human-readable layer

_(to follow as the JSON import/export layer goes in.)_
