/*
 * cbor_encoder.h — Conexio Cloud SDK CBOR encoding helpers
 *
 * Copyright (c) 2026 Conexio Technologies, Inc
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Thin chainable wrapper around Zephyr's zcbor library (already present in
 * nRF Connect SDK — no extra dependencies required).
 *
 *
 * Usage pattern:
 *
 *   uint8_t buf[256];
 *   ZCBOR_STATE_E(zse, 8, buf, sizeof(buf), 1);  // 1 top-level map
 *
 *   cbor_encoder_t enc = CBOR_ENCODER_INIT(zse);
 *
 *   // Envelope fields
 *   enc = cbor_add_tstr(enc, "dev_id", device_id);
 *   enc = cbor_add_tstr(enc, "ts",     timestamp);
 *   enc = cbor_add_uint32(enc, "seq",  seq_num);
 *
 *   // Metrics map
 *   enc = cbor_start_map(enc, "metrics", 4);
 *   enc = cbor_add_int32(enc,  "temp",   -42);
 *   enc = cbor_add_float32(enc, "humidity", 65.4f);
 *   enc = cbor_add_uint32(enc, "_rssi",  rssi);
 *   enc = cbor_add_bool(enc,   "_active", true);
 *   enc = cbor_end_map(enc, 4);
 *
 *   if (!enc.ok) {
 *       // encoding failed — buffer too small or zcbor state error
 *   }
 *
 *   size_t cbor_len = zse->payload - buf;
 *   // cbor_len bytes in buf are the encoded payload
 *
 * Notes:
 *   - ZCBOR_STATE_E macro: last arg is the initial map/array nesting level.
 *     Pass 1 for a top-level map (the outer payload map).
 *     Add +1 for each nested map level used.
 *   - max_pairs in cbor_start_map / cbor_end_map must match exactly.
 *   - All functions are inline — no .c file needed.
 *   - Thread-safe: state is in the caller-allocated zcbor_state_t.
 */

#ifndef CONEXIO_CBOR_ENCODER_H
#define CONEXIO_CBOR_ENCODER_H

#include <zcbor_encode.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ── Encoder state ──────────────────────────────────────────────────────── */

/**
 * Chainable encoder state.
 * Pass by value through a chain of cbor_add_* calls.
 * If enc.ok becomes false, all subsequent calls are no-ops.
 */
typedef struct {
	zcbor_state_t *zse;
	bool ok;
} cbor_encoder_t;

/** Initialise an encoder from an existing zcbor_state_t */
#define CBOR_ENCODER_INIT(zse_ptr) { .zse = (zse_ptr), .ok = true }

/* ── Internal helper ────────────────────────────────────────────────────── */

static inline bool _cbor_put_key(zcbor_state_t *zse, const char *key)
{
	if (!key) return false;
	return zcbor_tstr_put_term(zse, key, strlen(key));
}

/* ── Map operations ─────────────────────────────────────────────────────── */

/**
 * Start a named nested map.
 * @param max_pairs  Number of key-value pairs that will be encoded inside.
 *                   Must match the corresponding cbor_end_map() call exactly.
 */
static inline cbor_encoder_t cbor_start_map(cbor_encoder_t enc,
					     const char *key,
					     size_t max_pairs)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_map_start_encode(enc.zse, max_pairs);
	}
	return enc;
}

/**
 * Close a nested map opened with cbor_start_map().
 */
static inline cbor_encoder_t cbor_end_map(cbor_encoder_t enc, size_t max_pairs)
{
	if (enc.ok) {
		enc.ok = zcbor_map_end_encode(enc.zse, max_pairs);
	}
	return enc;
}

/* ── Text string ────────────────────────────────────────────────────────── */

/**
 * Add a key-value pair where the value is a UTF-8 text string.
 */
static inline cbor_encoder_t cbor_add_tstr(cbor_encoder_t enc,
					    const char *key,
					    const char *val)
{
	if (enc.ok && key && val) {
		enc.ok = zcbor_tstr_put_term(enc.zse, key, strlen(key)) &&
			 zcbor_tstr_put_term(enc.zse, val, strlen(val));
	}
	return enc;
}

/* ── Floating point ─────────────────────────────────────────────────────── */

/**
 * Add a float16 value (IEEE 754 half-precision, 2 bytes in CBOR).
 * Best for values where 3 significant figures are enough (humidity, temp).
 */
static inline cbor_encoder_t cbor_add_float16(cbor_encoder_t enc,
					       const char *key,
					       float value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_float16_put(enc.zse, value);
	}
	return enc;
}

/**
 * Add a float32 value (IEEE 754 single-precision, 4 bytes in CBOR).
 */
static inline cbor_encoder_t cbor_add_float32(cbor_encoder_t enc,
					       const char *key,
					       float value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_float32_put(enc.zse, value);
	}
	return enc;
}

/**
 * Add a float64 value (IEEE 754 double-precision, 8 bytes in CBOR).
 * Use sparingly — 8 bytes. Prefer float16/float32 where precision allows.
 */
static inline cbor_encoder_t cbor_add_float64(cbor_encoder_t enc,
					       const char *key,
					       double value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_float64_put(enc.zse, value);
	}
	return enc;
}

/* ── Integer types ──────────────────────────────────────────────────────── */

/** Add a signed 32-bit integer value */
static inline cbor_encoder_t cbor_add_int32(cbor_encoder_t enc,
					     const char *key,
					     int32_t value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_int32_put(enc.zse, value);
	}
	return enc;
}

/** Add a signed 64-bit integer value */
static inline cbor_encoder_t cbor_add_int64(cbor_encoder_t enc,
					     const char *key,
					     int64_t value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_int64_put(enc.zse, value);
	}
	return enc;
}

/** Add an unsigned 32-bit integer value */
static inline cbor_encoder_t cbor_add_uint32(cbor_encoder_t enc,
					      const char *key,
					      uint32_t value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_uint32_put(enc.zse, value);
	}
	return enc;
}

/** Add an unsigned 64-bit integer value */
static inline cbor_encoder_t cbor_add_uint64(cbor_encoder_t enc,
					      const char *key,
					      uint64_t value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_uint64_put(enc.zse, value);
	}
	return enc;
}

/* ── Boolean ────────────────────────────────────────────────────────────── */

/** Add a boolean value */
static inline cbor_encoder_t cbor_add_bool(cbor_encoder_t enc,
					    const char *key,
					    bool value)
{
	if (enc.ok) {
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_bool_put(enc.zse, value);
	}
	return enc;
}

/* ── Binary data ────────────────────────────────────────────────────────── */

/** Add a raw binary byte string (bstr) */
static inline cbor_encoder_t cbor_add_bstr(cbor_encoder_t enc,
					    const char *key,
					    const uint8_t *data,
					    size_t len)
{
	if (enc.ok) {
		struct zcbor_string zs = { .value = data, .len = len };
		enc.ok = _cbor_put_key(enc.zse, key) &&
			 zcbor_bstr_encode(enc.zse, &zs);
	}
	return enc;
}

/* ── Status check ───────────────────────────────────────────────────────── */

/**
 * Check encoder status and log an error if encoding failed.
 * Returns true if encoding succeeded, false otherwise.
 */
static inline bool cbor_check_ok(cbor_encoder_t enc, const char *context)
{
	if (!enc.ok) {
		printk("CBOR encode failed: %s\n", context ? context : "unknown");
	}
	return enc.ok;
}

#endif /* CONEXIO_CBOR_ENCODER_H */
