# Design

This document captures the design decisions baked into the configuration
manager. Sections land as each layer of the system goes in. Right now
only the data model is described.

---

## Data model

The data model is the single source of truth for what configuration looks
like in RAM. It defines:

- one C struct per IO channel type (DI, DO, TC, AI, AO, PCNT, PWM)
- one struct for the device-wide system configuration
- typed enums for every constrained field
- factory-default tables, one per type

All of this lives in three headers and one source file:

```
src/application/
  config_limits.h     channel counts, name length
  config_types.h      enums + structs + io_domain_t
  config_defaults.h   extern decls of the default tables
  config_defaults.c   static const tables themselves
```

### Channel counts (`config_limits.h`)

Compile-time `#define`s, one per IO type. Drawn from the hardware diagram:

| Type | Count | Source on the board |
| ---- | ----: | -------------------- |
| DI   |    16 | 2 × PCA9555 |
| DO   |    16 | 2 × PCA9555 |
| TC   |     4 | MAX31856 array |
| AI   |     8 | ADC1.IN1..IN8 |
| AO   |     4 | DAC channels (placeholder, board light on this) |
| PCNT |     4 | TIM2.CH1..CH4 |
| PWM  |     4 | TIMn.CH1..CH4 |

`CONFIG_NAME_LEN = 16` (15 usable chars + null terminator). Matches the
length conventions used in CiA 401 device-info strings and fits cleanly
inside one TLV record.

To grow a channel count, bump the number and rebuild. The defaults tables
use array range-init so they scale without touching each index by hand.

### Type-by-type field summary

| Type | Fields | sizeof (host) |
| ---- | ------ | -------------: |
| `di_config_t`     | name, id, debounce_ms, polarity, fault_state, interrupt_enabled | 32 |
| `do_config_t`     | name, id, polarity, fault_state | 28 |
| `tc_config_t`     | name, id, tc_type, unit, cjc_enabled, filter_ms, fault_state, fault_value_c10 | 40 |
| `ai_config_t`     | name, id, input_mode, filter_ms, scale_num, scale_den, offset, fault_state, fault_value | 48 |
| `ao_config_t`     | name, id, output_mode, slew_per_s, scale_num, scale_den, offset, fault_state, fault_value | 48 |
| `pcnt_config_t`   | name, id, mode, edge, limit, reset_on_read | 36 |
| `pwm_config_t`    | name, id, period_us, duty_permille, fault_state, fault_duty_permille | 36 |
| `system_config_t` | canopen_node_id, can_bitrate, heartbeat_ms, sync_window_us, nmt_startup, producer_emcy_cob_id (see below) | 20 |

Host-side sizes are pinned by `tests/test_types.cpp` so silent growth
trips a unit test before it overruns the EEPROM budget. Cortex-M numbers
will differ in padding and get re-baselined on the target build.

### Notable field choices

#### Names are fixed-size `char[16]`

15 usable characters plus a null terminator. Fixed-size means no
allocator, no separate length field on flash, and direct mapping to a TLV
record. Names longer than 15 bytes are rejected at the setter boundary
(once the API exists in a later round).

#### IDs are `uint16_t`

Mirrors the CANopen object-index width (16-bit OD indices, 8-bit sub-
indices). One physical record = one `(domain, index)` pair.

#### AI / AO scaling is integer (num, den, offset)

```
engineering_value = (raw * scale_num) / scale_den + offset
```

Stored as three `int32_t` fields rather than `float scale, float offset`.
This avoids an FPU dependency on Cortex-M cores that ship without one and
gives deterministic results across build configurations and toolchain
versions. The triple maps cleanly onto CiA 401's analog-input PV
factor/offset objects (0x6126 / 0x6127) so the eventual CANopen object
dictionary mapping is one field per sub-index.

#### `fault_state_t` is shared across DI / DO / PWM

Three variants — `HOLD`, `LOW`, `HIGH` — covering the
hold-last/force-off/force-on semantics that the README calls for. The
*encoding* matters:

```
FAULT_STATE_HOLD  = 0
FAULT_STATE_LOW   = 1
FAULT_STATE_HIGH  = 2
```

`HOLD` is the zero value so any record that ends up zero-initialised
(fresh RAM cache, partial deserialisation, factory reset path) lands in
the safest state by default. The `Types.FaultStateHoldIsZero` test pins
this invariant.

For analog types (AI, AO, TC) the `LOW`/`HIGH` semantics don't map, so
those structs carry an additional `fault_value` field (`int32_t` for AI
and AO, `int16_t fault_value_c10` for TC in tenths of degrees). The
fallback value is applied when `fault_state != HOLD`.

#### Enums everywhere; every enum has a `_COUNT` sentinel

Constrained fields are typed enums (`tc_type_t`, `pcnt_edge_t`,
`can_bitrate_t`, etc.) rather than raw `uint8_t`. The `_COUNT` sentinel
at the end of each enum lets range checks read as

```c
EXPECT_LT(x.tc_type, TC_TYPE_COUNT);
```

rather than hard-coding the value of the last legal entry. The unit
tests use this; the setter validators will too.

#### `producer_emcy_cob_id` uses a sentinel

The CANopen predefined-connection-set default for the EMCY producer
COB-ID (object 0x1014) is `0x80 + node_id`, not a fixed literal value. To
keep that default consistent with `canopen_node_id` automatically:

- `producer_emcy_cob_id == 0` is a **sentinel**: derive `0x80 + node_id` at
  NMT startup.
- Any non-zero value is an **operator override** and used verbatim.

Storing the literal `0x80 + node_id` in the defaults table would silently
go stale if the node id is changed later; the sentinel eliminates the
coupling entirely. The same pattern generalises to the predefined PDO
COB-IDs (`0x180 + node_id` for TPDO1, etc.) when those fields land.

#### `io_domain_t`

A separate top-level enum that addresses an IO record by *type*:

```
IO_DOMAIN_DI   = 0
IO_DOMAIN_DO   = 1
IO_DOMAIN_TC   = 2
IO_DOMAIN_AI   = 3
IO_DOMAIN_AO   = 4
IO_DOMAIN_PCNT = 5
IO_DOMAIN_PWM  = 6
```

Values are deliberately stable. Appending to the end preserves prior
values, which is necessary if older firmware ever needs to read a config
written by a newer firmware revision.

### Factory defaults

`config_defaults.{h,c}` holds one `const` table per IO type plus
`g_system_defaults`. Tables have external linkage (declared `extern const`
in the header), so every translation unit reads the same definition. The
`const` qualifier keeps them in flash on the target (not RAM), and every
field is initialised explicitly so the array contents survive any future
compiler aggressiveness around partial initialisation.

Tables are populated using the gcc/clang range-init extension:

```c
const di_config_t g_di_defaults[CONFIG_NUM_DI] = {
    [0 ... CONFIG_NUM_DI - 1] = {
        .name              = "",
        .debounce_ms       = 10,
        .polarity          = DI_POLARITY_ACTIVE_HIGH,
        .fault_state       = FAULT_STATE_HOLD,
        .interrupt_enabled = false,
    },
};
```

This isn't strict ISO C, so `-Wpedantic` rejects it. The deviation is
documented and suppressed *locally* in `config_defaults.c` only, via

```c
#pragma clang diagnostic ignored "-Wgnu-designator"
#pragma GCC   diagnostic ignored "-Wpedantic"
```

so the rest of the codebase keeps full strict-warning enforcement.
Alternatives considered:

- Per-index spelling, e.g. `[0] = { ... }, [1] = { ... }, ..., [15] = { ... }`.
  Tedious for 16-channel arrays and error-prone (silent zero-fill on
  forgotten indices is the dragon here).
- A runtime `config_defaults_init()` that fills mutable arrays at boot.
  Loses the `static const` placement in flash, which is the property
  worth preserving on the target.

Defaults will be exercised:

1. On a fresh device (both EEPROM slots blank or corrupt → load defaults).
2. On an explicit `config_reset_defaults()` call.
3. Per-field, when a record loaded from flash predates the addition of a
   new field and the field has no value yet.

The defaults choices themselves are conservative: empty names, ID 0,
`FAULT_STATE_HOLD` for inputs, `FAULT_STATE_LOW` for outputs (off is the
safer side for solid-state and relay outputs), 100 ms TC filter, integer
scaling = 1/1 + 0 (i.e. raw-to-engineering passthrough), 100 Hz PWM at 0%
duty, CAN 500 kbit/s, CANopen node 1, NMT wait-for-command on boot.

### Test coverage

`tests/test_types.cpp` covers four categories, 12 tests total:

1. **Sizeof bounds** — `EXPECT_LE` ceiling on each struct, baselined to
   the current host-side size. Crossing the ceiling fires the test;
   raising it is a conscious act (bump the number, note the on-flash
   impact in the commit). Cortex-M numbers will differ in padding.
2. **Enum range** — every `_COUNT` sentinel must be positive.
3. **Defaults sanity** — every entry in every defaults table has a
   null-terminated name, in-range enum values, non-zero `scale_den` for
   AI/AO, valid duty for PWM, etc.
4. **Invariant pin tests** — `FAULT_STATE_HOLD == 0` and
   `IO_DOMAIN_COUNT == 7`. Either failure means somebody touched a
   foundational assumption and needs to revisit the rest of the codebase.

### Modularity story (today)

- **More channels of an existing type** — bump the relevant
  `CONFIG_NUM_*` and rebuild. Defaults tables scale via range-init; tests
  and consumers index off the same macros.
- **A new IO type** — touch four places: add the struct in
  `config_types.h`, add the channel count in `config_limits.h`, add the
  defaults table in `config_defaults.c`, extend the value of
  `IO_DOMAIN_COUNT` in the unit test. The eventual TLV codec and JSON
  emitter will add a fifth and sixth.
- **Board variants** — not addressed yet. When a second hardware spin
  appears, switch to a board-variant header that overrides any
  `CONFIG_NUM_*` it cares about; the file structure already supports it
  with minimal change.

### Non-goals at this layer

- No allocation. No I/O. No API. No threading. No persistence. Those
  belong to the layers built on top.
- No CAN-specific wire encoding — `system_config_t` describes *what* the
  device should do with CAN, not the raw object dictionary layout. The
  object dictionary mapping is a separate concern that will reuse these
  structs.
- No active "fault detection" logic. Fault-state fields describe
  *behaviour when a fault is signalled*, not how a fault is detected.

---

## Storage layer

_(to follow as the storage / slot layer goes in.)_

## API and threading

_(to follow as the manager API goes in.)_

## Human-readable layer

_(to follow as the JSON import/export layer goes in.)_
