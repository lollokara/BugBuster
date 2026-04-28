// =============================================================================
// script_storage.cpp — POSIX-based SPIFFS script file storage.
//
// SPIFFS flat namespace: literal paths /scripts/<name> (dedicated scripts partition)
// No rename() support on SPIFFS — writes go directly to the final path.
// =============================================================================

#include "script_storage.h"

#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>

// Base directory for all scripts on the dedicated scripts SPIFFS partition.
#define SCRIPTS_BASE "/scripts"

// ---------------------------------------------------------------------------
// script_storage_validate_name
// ---------------------------------------------------------------------------

bool script_storage_validate_name(const char *name)
{
    if (!name) return false;

    size_t len = strlen(name);
    if (len < 1 || len > SCRIPT_NAME_MAX) return false;

    // Must not start with '.'
    if (name[0] == '.') return false;

    // Must end with ".py"
    if (len < 3) return false;
    if (strcmp(name + len - 3, ".py") != 0) return false;

    // All characters must be [A-Za-z0-9_.-]
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '.' && c != '-') {
            return false;
        }
        // Reject control chars, whitespace, path separators, NUL
        if (c == '/' || c == '\\' || c == '\0' || isspace((unsigned char)c)) {
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// script_storage_resolve_path
// ---------------------------------------------------------------------------

bool script_storage_resolve_path(const char *name, char *out_path, size_t path_size)
{
    if (!script_storage_validate_name(name)) return false;
    int written = snprintf(out_path, path_size, "%s/%s", SCRIPTS_BASE, name);
    return (written > 0 && (size_t)written < path_size);
}

// ---------------------------------------------------------------------------
// script_storage_save
// ---------------------------------------------------------------------------

bool script_storage_save(const char *name, const uint8_t *body, size_t len,
                         char *err, size_t err_size)
{
    char path[128];
    if (!script_storage_resolve_path(name, path, sizeof(path))) {
        snprintf(err, err_size, "invalid script name");
        return false;
    }

    if (len > SCRIPT_BODY_MAX) {
        snprintf(err, err_size, "script too large (%zu > %u)", len, SCRIPT_BODY_MAX);
        return false;
    }

    // Write directly to the final path (SPIFFS has no rename support).
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, err_size, "fopen failed: %s", strerror(errno));
        return false;
    }

    bool ok = true;
    if (len > 0) {
        size_t written = fwrite(body, 1, len, f);
        if (written != len) {
            snprintf(err, err_size, "fwrite failed: wrote %zu of %zu", written, len);
            ok = false;
        }
    }

    fclose(f);

    if (!ok) {
        // Attempt cleanup on partial write
        unlink(path);
    }

    return ok;
}

// ---------------------------------------------------------------------------
// script_storage_read
// ---------------------------------------------------------------------------

bool script_storage_read(const char *name, uint8_t *out_buf, size_t *out_len,
                         char *err, size_t err_size)
{
    char path[128];
    if (!script_storage_resolve_path(name, path, sizeof(path))) {
        snprintf(err, err_size, "invalid script name");
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(err, err_size, "not found: %s", strerror(errno));
        return false;
    }

    size_t file_size = (size_t)st.st_size;
    if (file_size > *out_len) {
        snprintf(err, err_size, "buffer too small: need %zu, have %zu", file_size, *out_len);
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "fopen failed: %s", strerror(errno));
        return false;
    }

    size_t rd = fread(out_buf, 1, file_size, f);
    fclose(f);

    if (rd != file_size) {
        snprintf(err, err_size, "fread partial: got %zu of %zu", rd, file_size);
        return false;
    }

    *out_len = rd;
    return true;
}

// ---------------------------------------------------------------------------
// script_storage_delete
// ---------------------------------------------------------------------------

bool script_storage_delete(const char *name, char *err, size_t err_size)
{
    char path[128];
    if (!script_storage_resolve_path(name, path, sizeof(path))) {
        snprintf(err, err_size, "invalid script name");
        return false;
    }

    if (unlink(path) != 0) {
        snprintf(err, err_size, "unlink failed: %s", strerror(errno));
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// script_storage_list
// ---------------------------------------------------------------------------

int script_storage_list(char names[][SCRIPT_NAME_MAX + 1], int max_count)
{
    if (!names || max_count <= 0) return 0;

    DIR *dir = opendir(SCRIPTS_BASE);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;

    while (count < max_count && (ent = readdir(dir)) != NULL) {
        // SPIFFS stores everything flat; the "name" as seen by readdir is the
        // full flat filename (e.g. "scripts/foo.py" on some builds, or just
        // "foo.py" on others).  Filter entries that are valid script names.
        const char *entry_name = ent->d_name;

        // Some SPIFFS VFS impls prefix the subdir in d_name; strip it.
        const char *prefix = "scripts/";
        if (strncmp(entry_name, prefix, strlen(prefix)) == 0) {
            entry_name += strlen(prefix);
        }

        if (!script_storage_validate_name(entry_name)) continue;

        strncpy(names[count], entry_name, SCRIPT_NAME_MAX);
        names[count][SCRIPT_NAME_MAX] = '\0';
        count++;
    }

    closedir(dir);
    return count;
}
