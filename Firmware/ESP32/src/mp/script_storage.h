#pragma once
// =============================================================================
// script_storage.h — SPIFFS-backed Python script file storage.
//
// SPIFFS uses a flat namespace.  All scripts are stored at literal paths of
// the form "/spiffs/scripts/<name>" where <name> is a validated filename.
//
// Name rules (validate_script_name):
//   - 1–32 characters
//   - Characters: [A-Za-z0-9_.-]
//   - Must end with ".py"
//   - Must not start with '.'
//   - No '/', '\\', NUL, or whitespace
//
// SPIFFS does not support rename().  All writes go directly to the final path.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum name length (characters, not including NUL terminator).
#define SCRIPT_NAME_MAX   32

// Maximum script body size (bytes).
#define SCRIPT_BODY_MAX   (32 * 1024)

// Maximum number of scripts that can be listed.
#define SCRIPT_LIST_MAX   64

// ---------------------------------------------------------------------------
// Name validation
// ---------------------------------------------------------------------------

/**
 * Return true if name is a valid script filename per the rules above.
 * name must be NUL-terminated.
 */
bool script_storage_validate_name(const char *name);

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

/**
 * Write the full SPIFFS path for name into out_path (size >= 128).
 * Returns false if name is invalid.
 */
bool script_storage_resolve_path(const char *name, char *out_path, size_t path_size);

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

/**
 * Write body[0..len) to /spiffs/scripts/<name>.
 * Overwrites any existing file with the same name.
 * Returns true on success; on failure populates err (max err_size bytes, NUL-terminated).
 */
bool script_storage_save(const char *name, const uint8_t *body, size_t len,
                         char *err, size_t err_size);

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

/**
 * Read the content of /spiffs/scripts/<name> into out_buf.
 * out_buf must be allocated by the caller with at least *out_len bytes.
 * On entry *out_len is the buffer capacity; on success *out_len is set to the
 * number of bytes read.
 * Returns true on success; on failure populates err.
 */
bool script_storage_read(const char *name, uint8_t *out_buf, size_t *out_len,
                         char *err, size_t err_size);

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

/**
 * Delete /spiffs/scripts/<name>.
 * Returns true on success; on failure populates err.
 */
bool script_storage_delete(const char *name, char *err, size_t err_size);

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

/**
 * Enumerate all scripts on SPIFFS.
 * Fills names[][SCRIPT_NAME_MAX+1] with up to max_count NUL-terminated names.
 * Returns the number of entries written (may be 0).
 */
int script_storage_list(char names[][SCRIPT_NAME_MAX + 1], int max_count);

#ifdef __cplusplus
}
#endif
