/*****************************************************************************
 * Module:  config_json
 * Purpose: Human-readable JSON wrapper over the configuration manager.
 *          Exports the current cache as JSON; imports JSON and applies
 *          each record through the same validating setters the C API
 *          uses (no second source of truth for validation).
 *
 *          Wire shape:
 *
 *            {
 *              "format": 2,
 *              "system": { ... },
 *              "di":   [ { "channel": 0, ... }, { "channel": 5, ... } ],
 *              "do":   [ { "channel": 2, ... } ],
 *              "tc":   [ ... ],
 *              "ai":   [ ... ],
 *              "ao":   [ ... ],
 *              "pcnt": [ ... ],
 *              "pwm":  [ ... ]
 *            }
 *
 *          Each record carries an explicit `"channel"` field naming the
 *          cache index it targets. Array order is purely presentational
 *          — to change one record you ship a one-element array, not a
 *          long list of leading nulls. Export emits a dense, ordered
 *          array (channels 0..N-1) for readability.
 *
 *          Records with a missing, non-integer, or out-of-range
 *          `"channel"` are rejected. Duplicate channel values inside
 *          the same array are rejected after the first (first wins);
 *          this surfaces operator typos through `first_error` instead
 *          of last-writes-wins.
 *
 *          Enums are encoded as strings ("ACTIVE_HIGH", "500K") rather
 *          than integers, so a human can read the file. Integer
 *          alternatives are also accepted on import for tool friendliness.
 *
 *          Partial-field updates within a record are supported: any
 *          field missing from an object (other than `"channel"`)
 *          retains the current cached value. The operator can ship a
 *          single-element array touching a single field.
 *
 *          Forward compatibility: unrecognised top-level keys are
 *          tolerated and reported in `unknown_keys` so the count
 *          surfaces to the operator. Unrecognised fields inside a
 *          record are silently ignored.
 *
 *          THREADING / ALLOCATION: cJSON uses heap (malloc) for parsing
 *          and building. This layer is intended for the External Comms
 *          / CLI thread that has its own heap allowance, NOT the IO hot
 *          path. The underlying config_get_* / config_set_* calls are
 *          mutex-protected.
 *****************************************************************************/

#ifndef APPLICATION_CONFIG_JSON_H
#define APPLICATION_CONFIG_JSON_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

#include "application/config.h"

typedef struct
{
    uint16_t accepted;     /* records successfully applied via setters */
    uint16_t rejected;     /* records we couldn't apply: missing/bad/
                              duplicate channel, patch failure, or
                              setter rejection */
    uint16_t unknown_keys; /* top-level keys we didn't recognise
                              (forward compat) */
    uint16_t malformed;    /* array entries that weren't an object or
                              null, or arrays where a key expected one */
    char first_error[96];  /* human-readable description of the first
                              failure; empty string if no failures */
} config_import_report_t;

/* Encode the current cache into `buf` as JSON. Returns CONFIG_OK on
 * success and writes the number of bytes (excluding the NUL terminator)
 * to *written. The buffer ALWAYS gets a trailing NUL on success.
 *
 * Returns:
 *   CONFIG_OK              success
 *   CONFIG_ERR_INVALID     buf == NULL or written == NULL
 *   CONFIG_ERR_TOO_LARGE   buf isn't big enough for the encoded form
 *   CONFIG_ERR_INTERNAL    cJSON couldn't allocate (heap exhausted) */
config_status_t config_export_json (char * buf, size_t cap, size_t * written);

/* Parse `json` (length `len`) and apply each known record through the
 * normal config_set_* validators. Optional `report` is populated with
 * a summary; pass NULL if you don't care.
 *
 * Sparse / partial semantics:
 *   - JSON top-level keys not in {format, system, di, do, tc, ai, ao,
 *     pcnt, pwm} are tolerated and counted in `unknown_keys`.
 *   - For arrays: only records that appear are touched. There is no
 *     positional implication — a record's effect on the cache is
 *     determined entirely by its `"channel"` field. `null` entries
 *     are accepted as silent no-ops.
 *   - Within a record object: missing fields (other than `"channel"`)
 *     keep the current cached value; unknown field names are silently
 *     ignored.
 *
 * Returns:
 *   CONFIG_OK              JSON parsed (individual records may still
 *                          have been rejected; check report->rejected)
 *   CONFIG_ERR_INVALID     json == NULL or root JSON is not an object
 *   CONFIG_ERR_CODEC       JSON parse error (malformed input) */
config_status_t config_import_json (const char *             json,
                                    size_t                   len,
                                    config_import_report_t * report);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_JSON_H */
