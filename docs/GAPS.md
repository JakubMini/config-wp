# Review findings — known gaps

Captured from a structured PRO/CON review of `main` against the README
requirements, `docs/DESIGN.md`, and the planned FW/HW architecture
diagrams. Each finding lists evidence, why it matters, and the
cheapest credible fix. Items the design explicitly defers (`Out of
scope`, `Non-goals`) are noted but not relitigated — see DESIGN.md.

## Summary

| Criterion           | Grade | Notes |
| ------------------- | ----- | ----- |
| Code clarity        | B+    | Naming + layering strong; mechanical repetition + a few opaque names. |
| Architecture        | A-    | Layering and concurrency pattern correct; OTA + real-EEPROM + field-level compat absent. |
| Decision docs       | A-    | Trade-off depth uneven; some magic numbers unjustified. |
| Requirements        | B+    | All 10 covered structurally; depth varies on req 2/4/5/6/7/9/10. |
| Error handling      | B     | Vocabulary excellent; inter-layer mapping collapses detail, fault paths untested. |

## Priority shortlist

The five items worth doing before calling the prototype
production-track. Ordered by cost-to-value.

1. **Storage-fault injection in slot tests.** `test_storage.cpp` is one
   trivial test; no test exercises `SLOT_ERR_STORAGE` propagation
   through `config_init` or a half-written `slot_write`. Add a mock
   storage driver that can inject `STORAGE_ERR_IO` at chosen offsets.
   Pins requirement 10 ("detect R/W errors") behaviourally rather than
   structurally. ~½ day.
2. **Concurrent `config_save` test.** Contract is "single-writer by
   convention" ([config.h:20-24](../src/application/config.h:20)); no
   test enforces it. Either add a test that races two savers and
   asserts a defined outcome (one wins, the other returns a known
   error), or add a debug-only guard that detects the race and
   asserts. Pins requirement 4 at the save boundary. ~½ day.
3. **Replace silent `s_unknown` overflow.** `preserve_unknown_locked`
   at [config.c:282-297](../src/application/config.c:282) drops
   records when the 1 KB buffer is full. Even returning a count of
   dropped records via `config_init`'s status would let the caller
   surface a "config truncated" warning. Today it's pure data loss.
   ~1-2 hours.
4. **Document magic constants.** Single-line rationales for
   `UNKNOWN_BUF_BYTES = 1024`
   ([config.c:66](../src/application/config.c:66)), the External Comms
   queue depth of 4, stack multiplier of 4, and the 100 ms back-
   pressure timeout
   ([external_comms.c](../src/external_comms.c)). Each is a
   reasonable engineering default; saying so in-line costs nothing
   and ends the next reviewer's question. ~1 hour.
5. **Format-migration sketch in DESIGN.md.** `format: 2` is the JSON
   wire version; `SLOT_FORMAT_VER` is the on-flash version. Both
   today reject mismatched versions wholesale. A paragraph in
   DESIGN.md describing the intended migration model (e.g. "on
   `format_ver` bump, the manager reads under the old codec, decodes
   into the new struct via a per-type migrate fn, then writes back")
   prevents the next maintainer from inventing it from scratch.
   ~1 hour.

## (a) Code clarity

### a.1 Opaque static names
**Where:** [config.c:68-72](../src/application/config.c:68).
`s_unknown`, `s_initialised`, `s_blob_buf` lean on the module-header
block comment for meaning. The `s_` prefix says "static", not "role".
**Fix:** rename to `s_unknown_records`, `s_init_done`, `s_save_scratch`.
Mechanical, zero risk.

### a.2 Magic 1024 without derivation
**Where:** `UNKNOWN_BUF_BYTES` at
[config.c:66](../src/application/config.c:66). DESIGN.md:637 says
"plenty for one or two future record types" but doesn't show the
arithmetic. A future PDO struct could easily exceed 1 KB.
**Fix:** one-line comment: `1024 = 4 records × ~250 bytes worst-case
(post-CiA-401 PDO record sizes)`. Or whatever the actual budget is.

### a.3 Silent overflow inside `preserve_unknown_locked`
**Where:**
[config.c:282-297](../src/application/config.c:282). Drops the record
with no return code, no log, no counter. The caller of `config_init`
never learns that forward-compat data was lost.
**Fix:** see priority item 3.

### a.4 Mechanical repetition without abstraction
**Where:** 14 near-identical getter/setter pairs
([config.c:737-1095](../src/application/config.c:737)); 8 encode
loops ([config.c:465-605](../src/application/config.c:465)); 11
enum-to-string switches
([config_json.c:67-230](../src/application/config_json.c:67)).
Defensible as "explicit beats clever" for safety-critical code, but
the file is ~1100 lines that could be ~400.
**Fix:** **don't refactor casually.** If/when the IO-type count grows,
revisit with an `X-macro` or a per-type dispatch table; the cost
asymmetry (12 files for a new type today) is the trigger documented
in DESIGN.md:518. Until then, leave it.

### a.5 `decode_blob_locked` has no contract header
**Where:**
[config.c:399-426](../src/application/config.c:399). Non-trivial
control flow (iterator → tag extraction → switch dispatch → unknown
preservation) with no `/**` describing the contract.
**Fix:** 4-line header comment: input contract, output contract, what
happens on each TLV error.

## (b) Architecture

### b.1 No OTA / firmware A-B coordination
The slot's `format_ver` gates layout; nothing gates *firmware*
version. If the bootloader rolls back firmware, old firmware reading
config written by new firmware quietly drops fields it doesn't know.
**Why it matters:** real device with OTA needs a "config schema
written by firmware ≥ X" stamp. Today the only signal is
`format_ver`, which moves at a different cadence than firmware.
**Fix:** add `fw_min_compatible_version` to `slot_header_t`. Two
bytes. Defer until OTA enters scope.

### b.2 Field-level forward compat absent
**Where:** [DESIGN.md:455-466](DESIGN.md:455). Adding a field to an
existing struct → old firmware reads short bytes → `TLV_ERR_TRUNCATED`
→ manager applies defaults for the *entire* record. Both old fields
and the new field are lost from that record on the next save by old
firmware.
**Status:** explicit non-goal. Surface here because the README's
"forward compatibility" requirement is broader than what's delivered.
**Fix when needed:** nested TLV-within-record, or add per-field
length-prefix. Significant work; only justified if the device sees
schema growth within an existing IO type.

### b.3 Storage stub doesn't model EEPROM constraints
**Where:** [drivers/storage.c](../src/drivers/storage.c). Bare
`memcpy` at any offset. Real EEPROM has page boundaries (typically
64 B), write granularity, ~5-10 ms page-erase latency, and finite
endurance.
**Why it matters:** the A/B slot protocol assumes byte-granular
atomic writes. Real EEPROM gives only page-level atomicity; a torn
*page* write can leave the slot in a state the CRC catches but the
recovery story (fall back to other slot) doesn't reason about
explicitly.
**Fix:** when the real driver lands, add a page-aware host stub for
tests (configurable page size, optional torn-write injection).
Until then, document the assumption at the top of
`drivers/storage.h`.

### b.4 FreeRTOS `lock_is_held_by_current_thread` TOCTOU
**Where:**
[config_lock_freertos.c:72-79](../src/application/config_lock_freertos.c:72).
Reads `s_owner` and compares to `xTaskGetCurrentTaskHandle()`
non-atomically. Narrow window, documented as diagnostic-only.
**Status:** acknowledged in code + DESIGN.md:657-659.
**Fix:** none required for diagnostic use. If this ever escalates to
enforcement, switch to a single atomic read of an owner-handle word.

### b.5 `external_comms.c` lives at `src/` not `src/application/`
Minor layering blur. The task is high-level glue that depends on the
manager; it's not a driver.
**Fix:** move to `src/application/external_comms.c` and update the
CMake target. ~10 min. Defer if not bothering anyone.

### b.6 Concurrent `config_save` undefined
Single-writer is documented but not enforced or tested. See priority
item 2.

### b.7 Dual-function pin model is record-level, not pin-level
Same physical GPIO addressable as DI[n] or DO[m] via separate
records — works for "what config does this pin run under?" but
there's no struct saying "GPIO5 = (DI, 3) today, (DO, 2) tomorrow".
**Status:** README's "may be dual/triple function" hints at this;
DESIGN.md:289 defers board-variant headers.
**Fix when needed:** a `pin_assignment_t[CONFIG_NUM_PINS]` array in
`system_config_t` mapping pins to (domain, index). Don't add until
a board actually demands it.

## (c) Decision documentation

### c.1 Numerical claims unjustified
**Where:** DESIGN.md:361 ("false-negative ~2.3 × 10⁻¹⁰") and 363
("~130 years at 1 write/s"). Both derivable (`2⁻³²` for the first,
`2³² / (86400 × 365)` for the second) but uncited.
**Fix:** one-line derivation each. ~5 min.

### c.2 Constants without rationale
`UNKNOWN_BUF_BYTES = 1024`, queue depth 4, stack mult 4, 100 ms back-
pressure timeout. Each is a reasonable engineering default; none has
the "why this number" written down. See priority item 4.

### c.3 Only one trade-off table
TLV vs Protobuf at DESIGN.md:469-483 is a model decision matrix. No
comparable table exists for cJSON vs jsmn (the JSON parser choice),
or for the lock-abstraction shape (two impls vs runtime-pluggable
backend).
**Fix:** add 2-3 line bullets for each material library/design
choice. Doesn't need to be a full table.

### c.4 CANopen alignment is structural, not behavioural
DESIGN.md:212-231 maps field widths to OD indices. No test or
runtime check confirms that the values produced would be accepted by
a real CANopen stack.
**Status:** README only asks for "align pretty well"; full
conformance is out of scope. Surface here so it's clear what the
alignment claim does and doesn't promise.
**Fix when needed:** a conformance test against an OD parser, or
integration with a known CANopen stack mock.

### c.5 Format-migration story missing
Both `format_ver` (slot header) and `"format": 2` (JSON) reject
unrecognised versions wholesale today. Migration is the obvious next
step; the design doc should sketch it so the next contributor
doesn't have to invent it cold. See priority item 5.

### c.6 `config_init` idempotency asserted but untested
Header comment says "Idempotent: subsequent calls return CONFIG_OK
without reloading". No test pins it.
**Fix:** one test in `test_config.cpp` that calls `config_init`
twice and asserts the cache is unchanged between calls. ~10 min.

## (d) Requirements

| Req | Verdict | Gap |
| --- | ------- | --- |
| 1. IO struct fields | MET | — |
| 2. CiA 401 alignment | PARTIAL | PDO comm/mapping (0x1400-0x1BFF), identity (0x1000/0x1008-100A/0x1018), error behaviour (0x1029) deferred. README only asks for alignment; deliverable is honest about the gap. |
| 3. System config | MET | — |
| 4. Re-entrant R/W | MET for getters/setters; PARTIAL for save | Concurrent save is "by convention". See priority item 2. |
| 5. Forward compatibility | PARTIAL | Record-level only; field-level adds truncate on downgrade. See b.2. |
| 6. Flexible / dual-function I/Os | WEAK | Record-level only; no pin-mapping struct. See b.7. |
| 7. Storage abstraction | PARTIAL | Interface is portable; stub doesn't model real EEPROM. See b.3. |
| 8. Human readable | MET | — |
| 9. Piece-wise OR monolithic | PARTIAL | Piece-wise *in cache*, monolithic *to EEPROM*. Per-channel persistence not supported; would multiply slot writes. Trade-off documented at DESIGN.md:604-605. |
| 10. Error detection + input validation | PARTIAL | Input validation thorough; storage-error path untested. See priority item 1. |

## (e) Error handling

### e.1 Inter-layer error collapse
`TLV_ERR_TRUNCATED`, `TLV_ERR_OVERRUN`, `TLV_ERR_TOO_BIG` all map to
a single `CONFIG_ERR_CODEC` at the public boundary. Operator seeing
"codec failed" can't tell whether the slot is corrupted at byte X or
a single record overflowed.
**Fix:** widen `config_status_t` with one or two more codes
(`CONFIG_ERR_CODEC_TRUNCATED`, `CONFIG_ERR_CODEC_OVERRUN`), or
return the underlying `tlv_status_t` via an out-parameter on
init/save. ~2 hours.

### e.2 `first_error` only captures the first failure
`config_import_report_t.first_error[96]` is a 96-byte buffer that
captures one failure. A 50-record JSON import with 5 bad records
returns `rejected=5` but operators only see one error message.
**Status:** explicit contract (DESIGN.md:778-786).
**Fix when wanted:** a fixed-size ring of last N errors, or a
callback the caller can hook for streaming diagnostics. Defer until
operators actually hit this.

### e.3 `s_unknown` overflow silent drop
See a.3 / priority item 3.

### e.4 Storage-fault paths untested
See priority item 1. `test_storage.cpp` is one trivial test; nothing
exercises what happens when `storage_read` fails mid-`slot_pick_active`
or `storage_write` fails halfway through `slot_write`.

### e.5 FreeRTOS lock-held check is diagnostic-only
See b.4. Acknowledged in code and design doc.

### e.6 Unknown JSON keys silently tolerated
A typo (`"systm"` for `"system"`) returns `CONFIG_OK` and the
operator sees no warning unless they read `report.unknown_keys`.
**Status:** intentional for forward compatibility.
**Optional:** add a strict-mode flag (`config_import_strict`) that
treats unknown keys as errors. Useful for CI checks on
hand-authored config files. ~1 hour.

## What this document is not

- **Not a request for refactoring.** The mechanical repetition in
  `config.c` is defensible; don't touch it until the IO-type count
  grows enough to trigger the cost-asymmetry threshold (DESIGN.md:518).
- **Not a rejection of design choices.** Most items here are
  deliberate trade-offs, sometimes documented as out-of-scope in
  DESIGN.md. Surfacing them is about traceability, not relitigation.
- **Not exhaustive.** Lints, header guards, and cosmetic style live
  in the build system; this document covers design-level gaps only.
