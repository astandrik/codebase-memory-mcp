/*
 * config_toml_edit.c — Fail-closed edits for agent TOML configuration.
 */
#include "cli/config_toml_edit.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "foundation/win_utf8.h"

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define toml_close _close
#define toml_fdopen _fdopen
#define TOML_SYNC _commit
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define toml_close close
#define toml_fdopen fdopen
#define TOML_SYNC fsync
#endif

#define TOML_EDIT_OK 0
#define TOML_EDIT_FOREIGN 1
#define TOML_EDIT_ERR (-1)
#define TOML_EDIT_MAX_BYTES (16U * 1024U * 1024U)
#define TOML_EDIT_MAX_PATH_BYTES 32768U

#ifdef CBM_TOML_EDIT_ENABLE_TEST_API
static CBM_TLS cbm_toml_precommit_test_hook_t toml_precommit_test_hook = NULL;
static CBM_TLS void *toml_precommit_test_context = NULL;
static CBM_TLS cbm_toml_precommit_test_hook_t toml_prepublish_test_hook = NULL;
static CBM_TLS void *toml_prepublish_test_context = NULL;
#endif

typedef struct {
    int exists;
#ifdef _WIN32
    DWORD volume_serial;
    DWORD file_index_high;
    DWORD file_index_low;
    DWORD attributes;
    DWORD link_count;
    FILETIME creation_time;
    FILETIME write_time;
    uint64_t size;
#else
    dev_t device;
    ino_t inode;
    mode_t mode;
    nlink_t link_count;
    uid_t owner;
    gid_t group;
    off_t size;
    int64_t modified_sec;
    long modified_nsec;
    int64_t changed_sec;
    long changed_nsec;
#endif
} toml_file_snapshot_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} toml_buffer_t;

typedef struct {
    size_t start;
    size_t content_end;
    size_t full_end;
} toml_line_t;

typedef struct {
    char *data;
    size_t len;
    size_t consumed;
} toml_string_t;

typedef struct {
    char *data;
    size_t len;
    size_t count;
} toml_key_path_t;

typedef struct {
    int present;
    int array;
    int target;
    size_t edit_start;
    toml_key_path_t path;
} toml_header_t;

typedef struct {
    int present;
    int multiline_value;
    toml_key_path_t key;
    size_t value_start;
    size_t value_end;
} toml_assignment_t;

typedef struct {
    int matching_count;
    size_t start;
    size_t header_end;
    size_t direct_end;
    size_t edit_end;
} toml_table_scan_t;

typedef struct {
    int active;
    int descendants;
    int identity_count;
    int identity_matches;
    size_t start;
    size_t header_end;
    size_t direct_end;
    size_t direct_significant_end;
    size_t last_significant_end;
} toml_target_table_t;

typedef struct {
    toml_key_path_t key;
    toml_line_t line;
} toml_body_entry_t;

typedef struct {
    toml_body_entry_t *entries;
    size_t count;
    size_t capacity;
} toml_body_spec_t;

static int toml_managed_block_conflicts(const char *existing, size_t existing_len,
                                        size_t exclude_start, size_t exclude_end, const char *block,
                                        size_t block_len);

static void toml_buffer_dispose(toml_buffer_t *buffer) {
    if (!buffer) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static int toml_buffer_reserve(toml_buffer_t *buffer, size_t additional) {
    if (!buffer || buffer->len == SIZE_MAX || additional > SIZE_MAX - buffer->len - 1) {
        return TOML_EDIT_ERR;
    }
    size_t needed = buffer->len + additional + 1;
    if (needed > (size_t)TOML_EDIT_MAX_BYTES + 1U) {
        return TOML_EDIT_ERR;
    }
    if (needed <= buffer->cap) {
        return TOML_EDIT_OK;
    }

    size_t cap = buffer->cap ? buffer->cap : 128;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    char *grown = (char *)realloc(buffer->data, cap);
    if (!grown) {
        return TOML_EDIT_ERR;
    }
    buffer->data = grown;
    buffer->cap = cap;
    return TOML_EDIT_OK;
}

static int toml_buffer_append(toml_buffer_t *buffer, const char *data, size_t len) {
    if ((!data && len != 0) || toml_buffer_reserve(buffer, len) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    if (len != 0) {
        memcpy(buffer->data + buffer->len, data, len);
        buffer->len += len;
    }
    buffer->data[buffer->len] = '\0';
    return TOML_EDIT_OK;
}

static int toml_buffer_append_char(toml_buffer_t *buffer, char value) {
    return toml_buffer_append(buffer, &value, 1);
}

static int toml_buffer_append_cstr(toml_buffer_t *buffer, const char *value) {
    return value ? toml_buffer_append(buffer, value, strlen(value)) : TOML_EDIT_ERR;
}

static int toml_utf8_is_valid(const char *text, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        unsigned char first = (unsigned char)text[pos++];
        if (first <= 0x7f) {
            continue;
        }
        size_t continuation_count;
        uint32_t codepoint;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation_count = 2;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation_count = 3;
            codepoint = first & 0x07;
        } else {
            return 0;
        }
        if (continuation_count > len - pos) {
            return 0;
        }
        for (size_t i = 0; i < continuation_count; ++i) {
            unsigned char next = (unsigned char)text[pos++];
            if ((next & 0xc0) != 0x80) {
                return 0;
            }
            codepoint = (codepoint << 6) | (uint32_t)(next & 0x3f);
        }
        if ((continuation_count == 1 && codepoint < 0x80) ||
            (continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return 0;
        }
    }
    return 1;
}

static int toml_text_is_safe(const char *text, size_t len, int multiline) {
    if (!text && len != 0) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == 0 || ch == 0x7f) {
            return 0;
        }
        if (ch < 0x20 && !(multiline && (ch == '\t' || ch == '\n' || ch == '\r'))) {
            return 0;
        }
        if (ch == '\r' && (!multiline || i + 1U >= len || text[i + 1U] != '\n')) {
            return 0;
        }
    }
    return toml_utf8_is_valid(text, len);
}

static int toml_bounded_length(const char *text, size_t maximum, size_t *length_out) {
    if (!text || !length_out) {
        return TOML_EDIT_ERR;
    }
    size_t length = 0U;
    while (length <= maximum && text[length] != '\0') {
        length++;
    }
    if (length > maximum) {
        return TOML_EDIT_ERR;
    }
    *length_out = length;
    return TOML_EDIT_OK;
}

static int toml_valid_path(const char *path) {
    size_t len = 0U;
    return path && path[0] != '\0' &&
           toml_bounded_length(path, TOML_EDIT_MAX_PATH_BYTES, &len) == TOML_EDIT_OK &&
           toml_text_is_safe(path, len, 0);
}

static int toml_is_bare_key_char(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-';
}

static int toml_valid_identifier(const char *identifier) {
    size_t length = 0U;
    if (!identifier || identifier[0] == '\0' ||
        toml_bounded_length(identifier, 4096U, &length) != TOML_EDIT_OK) {
        return 0;
    }
    for (size_t i = 0U; i < length; ++i) {
        if (!toml_is_bare_key_char((unsigned char)identifier[i])) {
            return 0;
        }
    }
    return 1;
}

static int toml_valid_marker(const char *marker) {
    if (!marker || marker[0] == '\0') {
        return 0;
    }
    size_t len = 0U;
    if (toml_bounded_length(marker, 4096U, &len) != TOML_EDIT_OK) {
        return 0;
    }
    return toml_text_is_safe(marker, len, 0) && !strchr(marker, '\n') && !strchr(marker, '\r');
}

static int toml_snapshot_equal(const toml_file_snapshot_t *left,
                               const toml_file_snapshot_t *right) {
    if (left->exists != right->exists) {
        return 0;
    }
    if (!left->exists) {
        return 1;
    }
#ifdef _WIN32
    return left->volume_serial == right->volume_serial &&
           left->file_index_high == right->file_index_high &&
           left->file_index_low == right->file_index_low && left->attributes == right->attributes &&
           left->link_count == right->link_count &&
           left->creation_time.dwLowDateTime == right->creation_time.dwLowDateTime &&
           left->creation_time.dwHighDateTime == right->creation_time.dwHighDateTime &&
           left->write_time.dwLowDateTime == right->write_time.dwLowDateTime &&
           left->write_time.dwHighDateTime == right->write_time.dwHighDateTime &&
           left->size == right->size;
#else
    return left->device == right->device && left->inode == right->inode &&
           left->mode == right->mode && left->link_count == right->link_count &&
           left->owner == right->owner && left->group == right->group &&
           left->size == right->size && left->modified_sec == right->modified_sec &&
           left->modified_nsec == right->modified_nsec && left->changed_sec == right->changed_sec &&
           left->changed_nsec == right->changed_nsec;
#endif
}

#ifdef _WIN32
static int toml_snapshot_from_handle(HANDLE handle, toml_file_snapshot_t *snapshot) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nNumberOfLinks != 1U || (info.nFileIndexHigh == 0U && info.nFileIndexLow == 0U)) {
        return TOML_EDIT_ERR;
    }
    uint64_t size = ((uint64_t)info.nFileSizeHigh << 32U) | (uint64_t)info.nFileSizeLow;
    if (size > TOML_EDIT_MAX_BYTES) {
        return TOML_EDIT_ERR;
    }
    *snapshot = (toml_file_snapshot_t){
        .exists = 1,
        .volume_serial = info.dwVolumeSerialNumber,
        .file_index_high = info.nFileIndexHigh,
        .file_index_low = info.nFileIndexLow,
        .attributes = info.dwFileAttributes,
        .link_count = info.nNumberOfLinks,
        .creation_time = info.ftCreationTime,
        .write_time = info.ftLastWriteTime,
        .size = size,
    };
    return TOML_EDIT_OK;
}
#else
static int toml_snapshot_from_stat(const struct stat *state, toml_file_snapshot_t *snapshot) {
    if (!S_ISREG(state->st_mode) || state->st_ino == 0 || state->st_nlink != 1U ||
        state->st_size < 0 || (uint64_t)state->st_size > TOML_EDIT_MAX_BYTES ||
        (state->st_mode & (S_ISUID | S_ISGID | S_ISVTX)) != 0) {
        return TOML_EDIT_ERR;
    }
    *snapshot = (toml_file_snapshot_t){
        .exists = 1,
        .device = state->st_dev,
        .inode = state->st_ino,
        .mode = state->st_mode,
        .link_count = state->st_nlink,
        .owner = state->st_uid,
        .group = state->st_gid,
        .size = state->st_size,
#ifdef __APPLE__
        .modified_sec = state->st_mtimespec.tv_sec,
        .modified_nsec = state->st_mtimespec.tv_nsec,
        .changed_sec = state->st_ctimespec.tv_sec,
        .changed_nsec = state->st_ctimespec.tv_nsec,
#else
        .modified_sec = state->st_mtim.tv_sec,
        .modified_nsec = state->st_mtim.tv_nsec,
        .changed_sec = state->st_ctim.tv_sec,
        .changed_nsec = state->st_ctim.tv_nsec,
#endif
    };
    return TOML_EDIT_OK;
}
#endif

static int toml_read_file(const char *path, char **out_data, size_t *out_len,
                          toml_file_snapshot_t *snapshot_out) {
    if (!path || !out_data || !out_len || !snapshot_out) {
        return TOML_EDIT_ERR;
    }
    *out_data = NULL;
    *out_len = 0;
    memset(snapshot_out, 0, sizeof(*snapshot_out));

#ifdef _WIN32
    wchar_t *wide_path = cbm_utf8_to_wide(path);
    if (!wide_path) {
        return TOML_EDIT_ERR;
    }
    HANDLE handle = CreateFileW(
        wide_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide_path);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return TOML_EDIT_ERR;
        }
        char *empty = (char *)malloc(1);
        if (!empty) {
            return TOML_EDIT_ERR;
        }
        empty[0] = '\0';
        *out_data = empty;
        return TOML_EDIT_OK;
    }
    toml_file_snapshot_t before;
    if (toml_snapshot_from_handle(handle, &before) != TOML_EDIT_OK) {
        CloseHandle(handle);
        return TOML_EDIT_ERR;
    }
    size_t size = (size_t)before.size;
    char *data = (char *)malloc(size + 1U);
    if (!data) {
        CloseHandle(handle);
        return TOML_EDIT_ERR;
    }
    DWORD read_count = 0U;
    BOOL read_ok = ReadFile(handle, data, (DWORD)size, &read_count, NULL);
    toml_file_snapshot_t after;
    int after_result = toml_snapshot_from_handle(handle, &after);
    BOOL close_ok = CloseHandle(handle);
    if (!read_ok || read_count != (DWORD)size || after_result != TOML_EDIT_OK || !close_ok ||
        !toml_snapshot_equal(&before, &after)) {
        free(data);
        return TOML_EDIT_ERR;
    }
#else
#ifndef O_NOFOLLOW
    return TOML_EDIT_ERR;
#else
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        if (errno != ENOENT) {
            return TOML_EDIT_ERR;
        }
        struct stat path_state;
        if (lstat(path, &path_state) == 0 || errno != ENOENT) {
            return TOML_EDIT_ERR;
        }
        char *empty = (char *)malloc(1U);
        if (!empty) {
            return TOML_EDIT_ERR;
        }
        empty[0] = '\0';
        *out_data = empty;
        return TOML_EDIT_OK;
    }
    struct stat before_state;
    toml_file_snapshot_t before;
    if (fstat(fd, &before_state) != 0 ||
        toml_snapshot_from_stat(&before_state, &before) != TOML_EDIT_OK) {
        toml_close(fd);
        return TOML_EDIT_ERR;
    }
    FILE *file = toml_fdopen(fd, "rb");
    if (!file) {
        toml_close(fd);
        return TOML_EDIT_ERR;
    }
    size_t size = (size_t)before.size;
    char *data = (char *)malloc(size + 1U);
    if (!data) {
        (void)fclose(file);
        return TOML_EDIT_ERR;
    }
    size_t read_count = size ? fread(data, 1, size, file) : 0;
    int read_error = ferror(file);
    struct stat after_state;
    toml_file_snapshot_t after;
    int after_result = fstat(cbm_fileno(file), &after_state) == 0
                           ? toml_snapshot_from_stat(&after_state, &after)
                           : TOML_EDIT_ERR;
    int close_error = fclose(file);
    if (read_count != size || read_error || close_error != 0 || after_result != TOML_EDIT_OK ||
        !toml_snapshot_equal(&before, &after)) {
        free(data);
        return TOML_EDIT_ERR;
    }
#endif
#endif
    data[size] = '\0';
    *out_data = data;
    *out_len = size;
    *snapshot_out = before;
    return TOML_EDIT_OK;
}

#ifndef _WIN32
static char *toml_parent_directory(const char *path) {
    const char *separator = strrchr(path, '/');
    if (!separator) {
        return cbm_strdup(".");
    }
    if (separator == path) {
        return cbm_strdup("/");
    }
    return cbm_strndup(path, (size_t)(separator - path));
}
#endif

static int toml_snapshot_matches_path(const char *path, const char *old_data, size_t old_len,
                                      const toml_file_snapshot_t *expected) {
    char *current = NULL;
    size_t current_len = 0U;
    toml_file_snapshot_t current_snapshot;
    if (toml_read_file(path, &current, &current_len, &current_snapshot) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    int matches = expected->exists == current_snapshot.exists &&
                  toml_snapshot_equal(expected, &current_snapshot) && current_len == old_len &&
                  (old_len == 0U || memcmp(current, old_data, old_len) == 0);
    free(current);
    return matches ? TOML_EDIT_OK : TOML_EDIT_ERR;
}

#ifndef _WIN32
static int toml_sync_parent_directory(const char *path) {
    char *parent = toml_parent_directory(path);
    if (!parent) {
        return TOML_EDIT_ERR;
    }
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(parent, flags);
    free(parent);
    if (fd < 0) {
        return TOML_EDIT_ERR;
    }
    struct stat state;
    int result = fstat(fd, &state) == 0 && S_ISDIR(state.st_mode) && fsync(fd) == 0 ? TOML_EDIT_OK
                                                                                    : TOML_EDIT_ERR;
    if (toml_close(fd) != 0) {
        result = TOML_EDIT_ERR;
    }
    return result;
}
#endif

static int toml_replace_atomic(const char *temp_path, const char *path, int existed) {
#ifdef _WIN32
    wchar_t *wide_temp = cbm_utf8_to_wide(temp_path);
    wchar_t *wide_path = cbm_utf8_to_wide(path);
    if (!wide_temp || !wide_path) {
        free(wide_temp);
        free(wide_path);
        return TOML_EDIT_ERR;
    }
    /* ReplaceFileW preserves the destination ACL and other mergeable metadata;
     * merge failures stay fatal so metadata is never silently discarded. */
    BOOL replaced =
        existed ? ReplaceFileW(wide_path, wide_temp, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL)
                : MoveFileExW(wide_temp, wide_path, MOVEFILE_WRITE_THROUGH);
    free(wide_temp);
    free(wide_path);
    return replaced ? TOML_EDIT_OK : TOML_EDIT_ERR;
#else
    if (!existed) {
        if (link(temp_path, path) != 0) {
            return TOML_EDIT_ERR;
        }
        if (cbm_unlink(temp_path) != 0) {
            return TOML_EDIT_ERR;
        }
        return toml_sync_parent_directory(path);
    }
    if (rename(temp_path, path) != 0) {
        return TOML_EDIT_ERR;
    }
    return toml_sync_parent_directory(path);
#endif
}

static int toml_write_atomic(const char *path, const char *old_data, size_t old_len,
                             const char *new_data, size_t new_len,
                             const toml_file_snapshot_t *snapshot) {
    if (old_len > TOML_EDIT_MAX_BYTES || new_len > TOML_EDIT_MAX_BYTES) {
        return TOML_EDIT_ERR;
    }
    if (old_len == new_len && (old_len == 0 || memcmp(old_data, new_data, old_len) == 0)) {
        return TOML_EDIT_OK;
    }
    size_t path_len = strlen(path);
    static const char suffix[] = ".XXXXXX";
    if (path_len > SIZE_MAX - sizeof(suffix)) {
        return TOML_EDIT_ERR;
    }
    char *temp_path = (char *)malloc(path_len + sizeof(suffix));
    if (!temp_path) {
        return TOML_EDIT_ERR;
    }
    memcpy(temp_path, path, path_len);
    memcpy(temp_path + path_len, suffix, sizeof(suffix));

    int fd = cbm_mkstemp(temp_path);
    if (fd < 0) {
        free(temp_path);
        return TOML_EDIT_ERR;
    }
    FILE *file = toml_fdopen(fd, "wb");
    if (!file) {
        (void)toml_close(fd);
        (void)cbm_unlink(temp_path);
        free(temp_path);
        return TOML_EDIT_ERR;
    }

    int failed = new_len != 0 && fwrite(new_data, 1, new_len, file) != new_len;
    if (!failed && fflush(file) != 0) {
        failed = 1;
    }
#ifndef _WIN32
    if (!failed && snapshot->exists &&
        fchown(cbm_fileno(file), snapshot->owner, snapshot->group) != 0) {
        failed = 1;
    }
    mode_t mode = snapshot->exists ? snapshot->mode & 0777U : 0600U;
    if (!failed && fchmod(cbm_fileno(file), mode) != 0) {
        failed = 1;
    }
#endif
    if (!failed && TOML_SYNC(cbm_fileno(file)) != 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)cbm_unlink(temp_path);
        free(temp_path);
        return TOML_EDIT_ERR;
    }
    char *temp_data = NULL;
    size_t temp_len = 0U;
    toml_file_snapshot_t temp_snapshot;
    if (toml_read_file(temp_path, &temp_data, &temp_len, &temp_snapshot) != TOML_EDIT_OK ||
        !temp_snapshot.exists || temp_len != new_len ||
        (new_len != 0U && memcmp(temp_data, new_data, new_len) != 0)) {
        free(temp_data);
        (void)cbm_unlink(temp_path);
        free(temp_path);
        return TOML_EDIT_ERR;
    }
    free(temp_data);
#ifdef CBM_TOML_EDIT_ENABLE_TEST_API
    if (toml_precommit_test_hook) {
        toml_precommit_test_hook(path, toml_precommit_test_context);
    }
#endif
    if (toml_snapshot_matches_path(path, old_data, old_len, snapshot) != TOML_EDIT_OK) {
        (void)cbm_unlink(temp_path);
        free(temp_path);
        return TOML_EDIT_ERR;
    }
#ifdef CBM_TOML_EDIT_ENABLE_TEST_API
    if (toml_prepublish_test_hook) {
        toml_prepublish_test_hook(path, toml_prepublish_test_context);
    }
#endif
    if (toml_snapshot_matches_path(path, old_data, old_len, snapshot) != TOML_EDIT_OK ||
        toml_snapshot_matches_path(temp_path, new_data, new_len, &temp_snapshot) != TOML_EDIT_OK ||
        toml_replace_atomic(temp_path, path, snapshot->exists) != TOML_EDIT_OK) {
        (void)cbm_unlink(temp_path);
        free(temp_path);
        return TOML_EDIT_ERR;
    }
    free(temp_path);
    return TOML_EDIT_OK;
}

#ifdef CBM_TOML_EDIT_ENABLE_TEST_API
void cbm_toml_set_precommit_hook_for_testing(cbm_toml_precommit_test_hook_t hook, void *context) {
    toml_precommit_test_hook = hook;
    toml_precommit_test_context = context;
}

void cbm_toml_set_prepublish_hook_for_testing(cbm_toml_precommit_test_hook_t hook, void *context) {
    toml_prepublish_test_hook = hook;
    toml_prepublish_test_context = context;
}
#endif

static int toml_next_line(const char *data, size_t len, size_t *cursor, toml_line_t *line) {
    if (!data || !cursor || !line || *cursor >= len) {
        return 0;
    }
    size_t pos = *cursor;
    line->start = pos;
    while (pos < len && data[pos] != '\n') {
        ++pos;
    }
    line->content_end = pos;
    if (line->content_end > line->start && data[line->content_end - 1] == '\r') {
        --line->content_end;
    }
    line->full_end = pos < len ? pos + 1 : pos;
    *cursor = line->full_end;
    return 1;
}

static int toml_line_equals(const char *data, const toml_line_t *line, const char *value) {
    size_t start = line->start;
    if (start == 0U && line->content_end >= 3U && (unsigned char)data[0] == 0xefU &&
        (unsigned char)data[1] == 0xbbU && (unsigned char)data[2] == 0xbfU) {
        start = 3U;
    }
    size_t line_len = line->content_end - start;
    size_t value_len = strlen(value);
    return line_len == value_len && memcmp(data + start, value, value_len) == 0;
}

enum {
    TOML_STRING_NONE = 0,
    TOML_STRING_MULTILINE_BASIC = 1,
    TOML_STRING_MULTILINE_LITERAL = 2,
};

static int toml_scan_line_strings(const char *data, const toml_line_t *line, int *multiline_state) {
    size_t pos = line->start;
    if (line->start == 0 && line->content_end >= 3U && (unsigned char)data[0] == 0xefU &&
        (unsigned char)data[1] == 0xbbU && (unsigned char)data[2] == 0xbfU) {
        pos = 3U;
    }
    while (pos < line->content_end) {
        if (*multiline_state != TOML_STRING_NONE) {
            char quote = *multiline_state == TOML_STRING_MULTILINE_BASIC ? '"' : '\'';
            if (*multiline_state == TOML_STRING_MULTILINE_BASIC && data[pos] == '\\') {
                pos += pos + 1U < line->content_end ? 2U : 1U;
                continue;
            }
            if (pos + 2U < line->content_end && data[pos] == quote && data[pos + 1U] == quote &&
                data[pos + 2U] == quote) {
                *multiline_state = TOML_STRING_NONE;
                pos += 3U;
                continue;
            }
            pos++;
            continue;
        }

        if (data[pos] == '#') {
            return TOML_EDIT_OK;
        }
        if (data[pos] != '"' && data[pos] != '\'') {
            pos++;
            continue;
        }
        char quote = data[pos];
        if (pos + 2U < line->content_end && data[pos + 1U] == quote && data[pos + 2U] == quote) {
            *multiline_state =
                quote == '"' ? TOML_STRING_MULTILINE_BASIC : TOML_STRING_MULTILINE_LITERAL;
            pos += 3U;
            continue;
        }
        pos++;
        int closed = 0;
        while (pos < line->content_end) {
            if (quote == '"' && data[pos] == '\\') {
                if (pos + 1U >= line->content_end) {
                    return TOML_EDIT_ERR;
                }
                pos += 2U;
                continue;
            }
            if (data[pos] == quote) {
                pos++;
                closed = 1;
                break;
            }
            pos++;
        }
        if (!closed) {
            return TOML_EDIT_ERR;
        }
    }
    return TOML_EDIT_OK;
}

static int toml_validate_lexical_strings(const char *data, size_t len) {
    size_t cursor = 0U;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    while (toml_next_line(data, len, &cursor, &line)) {
        if (toml_scan_line_strings(data, &line, &multiline_state) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
    }
    return multiline_state == TOML_STRING_NONE ? TOML_EDIT_OK : TOML_EDIT_ERR;
}

static int toml_find_markers(const char *data, size_t len, const char *begin_marker,
                             const char *end_marker, toml_line_t *begin_line, toml_line_t *end_line,
                             int *has_pair) {
    int begin_count = 0;
    int end_count = 0;
    size_t cursor = 0;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    while (toml_next_line(data, len, &cursor, &line)) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        if (!line_in_multiline && toml_line_equals(data, &line, begin_marker)) {
            ++begin_count;
            *begin_line = line;
        }
        if (!line_in_multiline && toml_line_equals(data, &line, end_marker)) {
            ++end_count;
            *end_line = line;
        }
        if (toml_scan_line_strings(data, &line, &multiline_state) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
    }
    if (multiline_state != TOML_STRING_NONE) {
        return TOML_EDIT_ERR;
    }
    if (begin_count == 0 && end_count == 0) {
        *has_pair = 0;
        return TOML_EDIT_OK;
    }
    if (begin_count != 1 || end_count != 1 || begin_line->start >= end_line->start) {
        return TOML_EDIT_ERR;
    }
    *has_pair = 1;
    return TOML_EDIT_OK;
}

static const char *toml_newline_style(const char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') {
            return i > 0U && data[i - 1U] == '\r' ? "\r\n" : "\n";
        }
    }
    return "\n";
}

static int toml_append_normalized_text(toml_buffer_t *output, const char *text, size_t len,
                                       const char *newline) {
    size_t cursor = 0U;
    toml_line_t line;
    while (toml_next_line(text, len, &cursor, &line)) {
        if (toml_buffer_append(output, text + line.start, line.content_end - line.start) !=
            TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
        if (line.full_end > line.content_end &&
            toml_buffer_append_cstr(output, newline) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
    }
    return TOML_EDIT_OK;
}

static int toml_contains_marker_line(const char *data, size_t len, const char *begin_marker,
                                     const char *end_marker) {
    size_t cursor = 0U;
    toml_line_t line;
    while (toml_next_line(data, len, &cursor, &line)) {
        if (toml_line_equals(data, &line, begin_marker) ||
            toml_line_equals(data, &line, end_marker)) {
            return 1;
        }
    }
    return 0;
}

static int toml_append_managed(toml_buffer_t *output, const char *begin_marker,
                               const char *end_marker, const char *block, const char *newline) {
    size_t block_len = strlen(block);
    if (toml_buffer_append_cstr(output, begin_marker) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, newline) != TOML_EDIT_OK ||
        toml_append_normalized_text(output, block, block_len, newline) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    if (block_len != 0 && block[block_len - 1] != '\n' &&
        toml_buffer_append_cstr(output, newline) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    return toml_buffer_append_cstr(output, end_marker) == TOML_EDIT_OK &&
                   toml_buffer_append_cstr(output, newline) == TOML_EDIT_OK
               ? TOML_EDIT_OK
               : TOML_EDIT_ERR;
}

int cbm_toml_escape_basic_string(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size == 0) {
        return TOML_EDIT_ERR;
    }
    out[0] = '\0';
    size_t input_len = 0U;
    if (toml_bounded_length(input, TOML_EDIT_MAX_BYTES, &input_len) != TOML_EDIT_OK ||
        !toml_utf8_is_valid(input, input_len)) {
        return TOML_EDIT_ERR;
    }
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        const char *escape = NULL;
        char unicode_escape[7];
        switch (*p) {
        case '\b':
            escape = "\\b";
            break;
        case '\t':
            escape = "\\t";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        default:
            if (*p < 0x20 || *p == 0x7f) {
                unicode_escape[0] = '\\';
                unicode_escape[1] = 'u';
                unicode_escape[2] = '0';
                unicode_escape[3] = '0';
                unicode_escape[4] = hex[*p >> 4];
                unicode_escape[5] = hex[*p & 0x0f];
                unicode_escape[6] = '\0';
                escape = unicode_escape;
            }
            break;
        }
        size_t add = escape ? strlen(escape) : 1;
        if (add > out_size - used - 1) {
            out[0] = '\0';
            return TOML_EDIT_ERR;
        }
        if (escape) {
            memcpy(out + used, escape, add);
        } else {
            out[used] = (char)*p;
        }
        used += add;
    }
    out[used] = '\0';
    return TOML_EDIT_OK;
}

int cbm_toml_upsert_managed_block(const char *file_path, const char *begin_marker,
                                  const char *end_marker, const char *block) {
    size_t block_len = 0U;
    if (!toml_valid_path(file_path) || !toml_valid_marker(begin_marker) ||
        !toml_valid_marker(end_marker) || strcmp(begin_marker, end_marker) == 0 || !block ||
        toml_bounded_length(block, TOML_EDIT_MAX_BYTES, &block_len) != TOML_EDIT_OK ||
        !toml_text_is_safe(block, block_len, 1) ||
        toml_contains_marker_line(block, block_len, begin_marker, end_marker) ||
        toml_validate_lexical_strings(block, block_len) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }

    char *existing = NULL;
    size_t existing_len = 0;
    toml_file_snapshot_t snapshot;
    if (toml_read_file(file_path, &existing, &existing_len, &snapshot) != TOML_EDIT_OK ||
        !toml_text_is_safe(existing, existing_len, 1)) {
        free(existing);
        return TOML_EDIT_ERR;
    }

    toml_line_t begin_line = {0};
    toml_line_t end_line = {0};
    int has_pair = 0;
    if (toml_find_markers(existing, existing_len, begin_marker, end_marker, &begin_line, &end_line,
                          &has_pair) != TOML_EDIT_OK) {
        free(existing);
        return TOML_EDIT_ERR;
    }
    size_t exclude_start = has_pair ? begin_line.start : SIZE_MAX;
    size_t exclude_end = has_pair ? end_line.full_end : SIZE_MAX;
    if (toml_managed_block_conflicts(existing, existing_len, exclude_start, exclude_end, block,
                                     block_len) != TOML_EDIT_OK) {
        free(existing);
        return TOML_EDIT_ERR;
    }

    toml_buffer_t output = {0};
    size_t prefix_len = has_pair ? begin_line.start : existing_len;
    if (has_pair && prefix_len == 0U && existing_len >= 3U && (unsigned char)existing[0] == 0xefU &&
        (unsigned char)existing[1] == 0xbbU && (unsigned char)existing[2] == 0xbfU) {
        prefix_len = 3U;
    }
    const char *newline = toml_newline_style(existing, existing_len);
    size_t payload_start = existing_len >= 3U && (unsigned char)existing[0] == 0xefU &&
                                   (unsigned char)existing[1] == 0xbbU &&
                                   (unsigned char)existing[2] == 0xbfU
                               ? 3U
                               : 0U;
    if (toml_buffer_append(&output, existing, prefix_len) != TOML_EDIT_OK ||
        (!has_pair && existing_len > payload_start && existing[existing_len - 1] != '\n' &&
         toml_buffer_append_cstr(&output, newline) != TOML_EDIT_OK) ||
        toml_append_managed(&output, begin_marker, end_marker, block, newline) != TOML_EDIT_OK ||
        (has_pair && toml_buffer_append(&output, existing + end_line.full_end,
                                        existing_len - end_line.full_end) != TOML_EDIT_OK)) {
        toml_buffer_dispose(&output);
        free(existing);
        return TOML_EDIT_ERR;
    }

    int result =
        toml_write_atomic(file_path, existing, existing_len, output.data, output.len, &snapshot);
    toml_buffer_dispose(&output);
    free(existing);
    return result;
}

int cbm_toml_remove_managed_block(const char *file_path, const char *begin_marker,
                                  const char *end_marker) {
    if (!toml_valid_path(file_path) || !toml_valid_marker(begin_marker) ||
        !toml_valid_marker(end_marker) || strcmp(begin_marker, end_marker) == 0) {
        return TOML_EDIT_ERR;
    }
    char *existing = NULL;
    size_t existing_len = 0;
    toml_file_snapshot_t snapshot;
    if (toml_read_file(file_path, &existing, &existing_len, &snapshot) != TOML_EDIT_OK ||
        !toml_text_is_safe(existing, existing_len, 1)) {
        free(existing);
        return TOML_EDIT_ERR;
    }

    toml_line_t begin_line = {0};
    toml_line_t end_line = {0};
    int has_pair = 0;
    if (toml_find_markers(existing, existing_len, begin_marker, end_marker, &begin_line, &end_line,
                          &has_pair) != TOML_EDIT_OK) {
        free(existing);
        return TOML_EDIT_ERR;
    }
    if (!has_pair) {
        free(existing);
        return TOML_EDIT_OK;
    }

    toml_buffer_t output = {0};
    size_t prefix_len = begin_line.start;
    if (prefix_len == 0U && existing_len >= 3U && (unsigned char)existing[0] == 0xefU &&
        (unsigned char)existing[1] == 0xbbU && (unsigned char)existing[2] == 0xbfU) {
        prefix_len = 3U;
    }
    if (toml_buffer_append(&output, existing, prefix_len) != TOML_EDIT_OK ||
        toml_buffer_append(&output, existing + end_line.full_end,
                           existing_len - end_line.full_end) != TOML_EDIT_OK) {
        toml_buffer_dispose(&output);
        free(existing);
        return TOML_EDIT_ERR;
    }
    int result =
        toml_write_atomic(file_path, existing, existing_len, output.data, output.len, &snapshot);
    toml_buffer_dispose(&output);
    free(existing);
    return result;
}

static int toml_hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int toml_append_codepoint(toml_buffer_t *buffer, uint32_t codepoint) {
    char encoded[4];
    size_t len = 0;
    if (codepoint == 0 || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return TOML_EDIT_ERR;
    }
    if (codepoint <= 0x7f) {
        encoded[0] = (char)codepoint;
        len = 1;
    } else if (codepoint <= 0x7ff) {
        encoded[0] = (char)(0xc0 | (codepoint >> 6));
        encoded[1] = (char)(0x80 | (codepoint & 0x3f));
        len = 2;
    } else if (codepoint <= 0xffff) {
        encoded[0] = (char)(0xe0 | (codepoint >> 12));
        encoded[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        encoded[2] = (char)(0x80 | (codepoint & 0x3f));
        len = 3;
    } else {
        encoded[0] = (char)(0xf0 | (codepoint >> 18));
        encoded[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        encoded[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        encoded[3] = (char)(0x80 | (codepoint & 0x3f));
        len = 4;
    }
    return toml_buffer_append(buffer, encoded, len);
}

static int toml_parse_unicode_escape(const char *text, size_t len, size_t digits,
                                     uint32_t *out_codepoint) {
    if (len < digits) {
        return TOML_EDIT_ERR;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < digits; ++i) {
        int digit = toml_hex_digit((unsigned char)text[i]);
        if (digit < 0) {
            return TOML_EDIT_ERR;
        }
        value = (value << 4) | (uint32_t)digit;
    }
    *out_codepoint = value;
    return TOML_EDIT_OK;
}

static int toml_parse_string(const char *text, size_t len, toml_string_t *parsed) {
    if (!text || !parsed || len < 2 || (text[0] != '"' && text[0] != '\'')) {
        return TOML_EDIT_ERR;
    }
    char quote = text[0];
    if (len >= 3 && text[1] == quote && text[2] == quote) {
        return TOML_EDIT_ERR;
    }
    toml_buffer_t value = {0};
    size_t pos = 1;
    while (pos < len) {
        unsigned char ch = (unsigned char)text[pos++];
        if (ch == (unsigned char)quote) {
            if (!value.data && toml_buffer_reserve(&value, 0) != TOML_EDIT_OK) {
                return TOML_EDIT_ERR;
            }
            parsed->data = value.data;
            parsed->len = value.len;
            parsed->consumed = pos;
            return TOML_EDIT_OK;
        }
        if (ch < 0x20 && ch != '\t') {
            toml_buffer_dispose(&value);
            return TOML_EDIT_ERR;
        }
        if (ch == 0x7f) {
            toml_buffer_dispose(&value);
            return TOML_EDIT_ERR;
        }
        if (quote == '\'' || ch != '\\') {
            if (toml_buffer_append_char(&value, (char)ch) != TOML_EDIT_OK) {
                toml_buffer_dispose(&value);
                return TOML_EDIT_ERR;
            }
            continue;
        }
        if (pos >= len) {
            toml_buffer_dispose(&value);
            return TOML_EDIT_ERR;
        }
        unsigned char escaped = (unsigned char)text[pos++];
        char decoded;
        switch (escaped) {
        case 'b':
            decoded = '\b';
            break;
        case 't':
            decoded = '\t';
            break;
        case 'n':
            decoded = '\n';
            break;
        case 'f':
            decoded = '\f';
            break;
        case 'r':
            decoded = '\r';
            break;
        case '"':
            decoded = '"';
            break;
        case '\\':
            decoded = '\\';
            break;
        case 'u':
        case 'U': {
            size_t digits = escaped == 'u' ? 4 : 8;
            uint32_t codepoint = 0;
            if (toml_parse_unicode_escape(text + pos, len - pos, digits, &codepoint) !=
                    TOML_EDIT_OK ||
                toml_append_codepoint(&value, codepoint) != TOML_EDIT_OK) {
                toml_buffer_dispose(&value);
                return TOML_EDIT_ERR;
            }
            pos += digits;
            continue;
        }
        default:
            toml_buffer_dispose(&value);
            return TOML_EDIT_ERR;
        }
        if (toml_buffer_append_char(&value, decoded) != TOML_EDIT_OK) {
            toml_buffer_dispose(&value);
            return TOML_EDIT_ERR;
        }
    }
    toml_buffer_dispose(&value);
    return TOML_EDIT_ERR;
}

static void toml_string_dispose(toml_string_t *value) {
    if (!value) {
        return;
    }
    free(value->data);
    value->data = NULL;
    value->len = 0;
    value->consumed = 0;
}

static void toml_key_path_dispose(toml_key_path_t *path) {
    if (!path) {
        return;
    }
    free(path->data);
    memset(path, 0, sizeof(*path));
}

static int toml_parse_key_path(const char *data, size_t start, size_t end, toml_key_path_t *path) {
    memset(path, 0, sizeof(*path));
    toml_buffer_t encoded = {0};
    size_t pos = start;
    while (pos < end) {
        while (pos < end && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++;
        }
        if (pos >= end || path->count >= 64U) {
            toml_buffer_dispose(&encoded);
            return TOML_EDIT_ERR;
        }
        if (data[pos] == '"' || data[pos] == '\'') {
            toml_string_t segment = {0};
            if (toml_parse_string(data + pos, end - pos, &segment) != TOML_EDIT_OK ||
                toml_buffer_append(&encoded, segment.data, segment.len) != TOML_EDIT_OK ||
                toml_buffer_append_char(&encoded, '\0') != TOML_EDIT_OK) {
                toml_string_dispose(&segment);
                toml_buffer_dispose(&encoded);
                return TOML_EDIT_ERR;
            }
            pos += segment.consumed;
            toml_string_dispose(&segment);
        } else {
            size_t segment_start = pos;
            while (pos < end && toml_is_bare_key_char((unsigned char)data[pos])) {
                pos++;
            }
            if (pos == segment_start ||
                toml_buffer_append(&encoded, data + segment_start, pos - segment_start) !=
                    TOML_EDIT_OK ||
                toml_buffer_append_char(&encoded, '\0') != TOML_EDIT_OK) {
                toml_buffer_dispose(&encoded);
                return TOML_EDIT_ERR;
            }
        }
        path->count++;
        while (pos < end && (data[pos] == ' ' || data[pos] == '\t')) {
            pos++;
        }
        if (pos == end) {
            break;
        }
        if (data[pos] != '.') {
            toml_buffer_dispose(&encoded);
            memset(path, 0, sizeof(*path));
            return TOML_EDIT_ERR;
        }
        pos++;
    }
    if (path->count == 0U) {
        toml_buffer_dispose(&encoded);
        return TOML_EDIT_ERR;
    }
    path->data = encoded.data;
    path->len = encoded.len;
    return TOML_EDIT_OK;
}

static const char *toml_key_path_segment(const toml_key_path_t *path, size_t index) {
    const char *segment = path->data;
    for (size_t i = 0U; i < index; ++i) {
        segment += strlen(segment) + 1U;
    }
    return segment;
}

static int toml_key_path_equal(const toml_key_path_t *left, const toml_key_path_t *right) {
    return left->count == right->count && left->len == right->len &&
           (left->len == 0U || memcmp(left->data, right->data, left->len) == 0);
}

static int toml_key_path_has_prefix(const toml_key_path_t *path, const toml_key_path_t *prefix) {
    if (path->count < prefix->count) {
        return 0;
    }
    for (size_t i = 0U; i < prefix->count; ++i) {
        if (strcmp(toml_key_path_segment(path, i), toml_key_path_segment(prefix, i)) != 0) {
            return 0;
        }
    }
    return 1;
}

static int toml_key_path_is_single(const toml_key_path_t *path, const char *name) {
    return path->count == 1U && strcmp(toml_key_path_segment(path, 0U), name) == 0;
}

static int toml_key_path_join(const toml_key_path_t *prefix, const toml_key_path_t *suffix,
                              toml_key_path_t *joined) {
    memset(joined, 0, sizeof(*joined));
    if (prefix->len > SIZE_MAX - suffix->len) {
        return TOML_EDIT_ERR;
    }
    joined->len = prefix->len + suffix->len;
    joined->data = (char *)malloc(joined->len + 1U);
    if (!joined->data) {
        return TOML_EDIT_ERR;
    }
    if (prefix->len != 0U) {
        memcpy(joined->data, prefix->data, prefix->len);
    }
    if (suffix->len != 0U) {
        memcpy(joined->data + prefix->len, suffix->data, suffix->len);
    }
    joined->data[joined->len] = '\0';
    joined->count = prefix->count + suffix->count;
    return TOML_EDIT_OK;
}

static void toml_trim(const char *data, size_t *start, size_t *end) {
    while (*start < *end && (data[*start] == ' ' || data[*start] == '\t')) {
        ++*start;
    }
    while (*end > *start && (data[*end - 1] == ' ' || data[*end - 1] == '\t')) {
        --*end;
    }
}

static int toml_parse_header(const char *data, const toml_line_t *line, const char *target_name,
                             toml_header_t *header) {
    memset(header, 0, sizeof(*header));
    size_t start = line->start;
    size_t end = line->content_end;
    header->edit_start = line->start;
    if (line->start == 0 && end - start >= 3 && (unsigned char)data[start] == 0xef &&
        (unsigned char)data[start + 1] == 0xbb && (unsigned char)data[start + 2] == 0xbf) {
        start += 3;
        header->edit_start = start;
    }
    while (start < end && (data[start] == ' ' || data[start] == '\t')) {
        ++start;
    }
    if (start >= end || data[start] != '[') {
        return TOML_EDIT_OK;
    }

    int array = start + 1 < end && data[start + 1] == '[';
    size_t inner_start = start + (array ? 2u : 1u);
    size_t pos = inner_start;
    char quote = '\0';
    int escaped = 0;
    size_t close_start = SIZE_MAX;
    size_t close_end = SIZE_MAX;
    while (pos < end) {
        char ch = data[pos];
        if (quote) {
            if (quote == '"' && !escaped && ch == '\\') {
                escaped = 1;
            } else {
                if (!escaped && ch == quote) {
                    quote = '\0';
                }
                escaped = 0;
            }
            ++pos;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            ++pos;
            continue;
        }
        if (ch == ']' && (!array || (pos + 1 < end && data[pos + 1] == ']'))) {
            close_start = pos;
            close_end = pos + (array ? 2u : 1u);
            break;
        }
        ++pos;
    }
    if (quote || close_start == SIZE_MAX) {
        return TOML_EDIT_ERR;
    }
    size_t inner_end = close_start;
    toml_trim(data, &inner_start, &inner_end);
    if (inner_start == inner_end) {
        return TOML_EDIT_ERR;
    }
    pos = close_end;
    while (pos < end && (data[pos] == ' ' || data[pos] == '\t')) {
        ++pos;
    }
    if (pos < end && data[pos] != '#') {
        return TOML_EDIT_ERR;
    }

    header->present = 1;
    header->array = array;
    if (toml_parse_key_path(data, inner_start, inner_end, &header->path) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    header->target = array && toml_key_path_is_single(&header->path, target_name);
    return TOML_EDIT_OK;
}

static void toml_header_dispose(toml_header_t *header) {
    if (header) {
        toml_key_path_dispose(&header->path);
    }
}

static int toml_line_is_blank_or_comment(const char *data, const toml_line_t *line) {
    size_t pos = line->start;
    while (pos < line->content_end && (data[pos] == ' ' || data[pos] == '\t')) {
        ++pos;
    }
    return pos == line->content_end || data[pos] == '#';
}

static int toml_parse_assignment(const char *data, const toml_line_t *line,
                                 toml_assignment_t *assignment) {
    memset(assignment, 0, sizeof(*assignment));
    size_t start = line->start;
    size_t end = line->content_end;
    if (start == 0U && end >= 3U && (unsigned char)data[0] == 0xefU &&
        (unsigned char)data[1] == 0xbbU && (unsigned char)data[2] == 0xbfU) {
        start = 3U;
    }
    while (start < end && (data[start] == ' ' || data[start] == '\t')) {
        start++;
    }
    if (start == end || data[start] == '#') {
        return TOML_EDIT_OK;
    }

    size_t equals = SIZE_MAX;
    char quote = '\0';
    int escaped = 0;
    for (size_t pos = start; pos < end; ++pos) {
        char ch = data[pos];
        if (quote) {
            if (quote == '"' && !escaped && ch == '\\') {
                escaped = 1;
            } else {
                if (!escaped && ch == quote) {
                    quote = '\0';
                }
                escaped = 0;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '=') {
            equals = pos;
            break;
        } else if (ch == '#') {
            break;
        }
    }
    if (quote || equals == SIZE_MAX) {
        return TOML_EDIT_OK;
    }
    size_t key_end = equals;
    toml_trim(data, &start, &key_end);
    if (toml_parse_key_path(data, start, key_end, &assignment->key) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }

    size_t value_start = equals + 1U;
    while (value_start < end && (data[value_start] == ' ' || data[value_start] == '\t')) {
        value_start++;
    }
    size_t value_end = end;
    quote = '\0';
    escaped = 0;
    for (size_t pos = value_start; pos < end; ++pos) {
        char ch = data[pos];
        if (quote) {
            if (quote == '"' && !escaped && ch == '\\') {
                escaped = 1;
            } else {
                if (!escaped && ch == quote) {
                    quote = '\0';
                }
                escaped = 0;
            }
            continue;
        }
        if ((ch == '"' || ch == '\'') && pos + 2U < end && data[pos + 1U] == ch &&
            data[pos + 2U] == ch) {
            assignment->multiline_value = 1;
            break;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '#') {
            value_end = pos;
            break;
        }
    }
    toml_trim(data, &value_start, &value_end);
    if (value_start == value_end) {
        toml_key_path_dispose(&assignment->key);
        return TOML_EDIT_ERR;
    }
    assignment->present = 1;
    assignment->value_start = value_start;
    assignment->value_end = value_end;
    return TOML_EDIT_OK;
}

static void toml_assignment_dispose(toml_assignment_t *assignment) {
    if (assignment) {
        toml_key_path_dispose(&assignment->key);
    }
}

static int toml_assignment_string_equals(const char *data, const toml_assignment_t *assignment,
                                         const char *expected, int *matches) {
    *matches = 0;
    if (!assignment->present || assignment->multiline_value) {
        return TOML_EDIT_ERR;
    }
    toml_string_t value = {0};
    if (toml_parse_string(data + assignment->value_start,
                          assignment->value_end - assignment->value_start,
                          &value) != TOML_EDIT_OK ||
        value.consumed != assignment->value_end - assignment->value_start) {
        toml_string_dispose(&value);
        return TOML_EDIT_ERR;
    }
    size_t expected_len = strlen(expected);
    *matches = value.len == expected_len && memcmp(value.data, expected, expected_len) == 0;
    toml_string_dispose(&value);
    return TOML_EDIT_OK;
}

static int toml_block_has_prior_table(const char *block, size_t block_len, size_t stop,
                                      const toml_key_path_t *desired, int desired_array) {
    size_t cursor = 0U;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    toml_key_path_t scope = {0};
    int inside_compatible_array = 0;
    while (toml_next_line(block, block_len, &cursor, &line) && line.start < stop) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        if (!line_in_multiline) {
            toml_header_t header;
            if (toml_parse_header(block, &line, "", &header) != TOML_EDIT_OK) {
                toml_key_path_dispose(&scope);
                return TOML_EDIT_ERR;
            }
            int duplicate = 0;
            if (header.present) {
                if (desired_array) {
                    int exact = toml_key_path_equal(&header.path, desired);
                    int descendant = header.path.count > desired->count &&
                                     toml_key_path_has_prefix(&header.path, desired);
                    if (exact) {
                        duplicate = !header.array;
                        inside_compatible_array = header.array;
                    } else if (descendant) {
                        duplicate = !inside_compatible_array;
                    } else {
                        inside_compatible_array = 0;
                    }
                } else {
                    duplicate = toml_key_path_has_prefix(&header.path, desired);
                }
                toml_key_path_dispose(&scope);
                scope = header.path;
                memset(&header.path, 0, sizeof(header.path));
            } else {
                toml_assignment_t assignment;
                if (toml_parse_assignment(block, &line, &assignment) != TOML_EDIT_OK) {
                    toml_header_dispose(&header);
                    toml_key_path_dispose(&scope);
                    return TOML_EDIT_ERR;
                }
                if (assignment.present) {
                    toml_key_path_t full_key;
                    if (toml_key_path_join(&scope, &assignment.key, &full_key) != TOML_EDIT_OK) {
                        toml_assignment_dispose(&assignment);
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&scope);
                        return TOML_EDIT_ERR;
                    }
                    duplicate =
                        !inside_compatible_array && (toml_key_path_has_prefix(&full_key, desired) ||
                                                     toml_key_path_has_prefix(desired, &full_key));
                    toml_key_path_dispose(&full_key);
                }
                toml_assignment_dispose(&assignment);
            }
            toml_header_dispose(&header);
            if (duplicate) {
                toml_key_path_dispose(&scope);
                return TOML_EDIT_ERR;
            }
        }
        if (toml_scan_line_strings(block, &line, &multiline_state) != TOML_EDIT_OK) {
            toml_key_path_dispose(&scope);
            return TOML_EDIT_ERR;
        }
    }
    toml_key_path_dispose(&scope);
    return TOML_EDIT_OK;
}

static int toml_existing_conflicts_with_table(const char *existing, size_t existing_len,
                                              size_t exclude_start, size_t exclude_end,
                                              const toml_key_path_t *desired, int desired_array) {
    size_t cursor = 0U;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    toml_key_path_t scope = {0};
    int inside_compatible_array = 0;
    while (toml_next_line(existing, existing_len, &cursor, &line)) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        int excluded =
            exclude_start != SIZE_MAX && line.start >= exclude_start && line.start < exclude_end;
        if (!line_in_multiline) {
            toml_header_t header;
            if (toml_parse_header(existing, &line, "", &header) != TOML_EDIT_OK) {
                toml_key_path_dispose(&scope);
                return TOML_EDIT_ERR;
            }
            if (header.present) {
                int conflict = 0;
                if (desired_array) {
                    int exact = toml_key_path_equal(&header.path, desired);
                    int descendant = header.path.count > desired->count &&
                                     toml_key_path_has_prefix(&header.path, desired);
                    if (exact) {
                        conflict = !excluded && !header.array;
                        inside_compatible_array = header.array;
                    } else if (descendant) {
                        conflict = !excluded && !inside_compatible_array;
                    } else {
                        inside_compatible_array = 0;
                    }
                } else {
                    conflict = !excluded && toml_key_path_has_prefix(&header.path, desired);
                }
                toml_key_path_dispose(&scope);
                scope = header.path;
                memset(&header.path, 0, sizeof(header.path));
                toml_header_dispose(&header);
                if (conflict) {
                    toml_key_path_dispose(&scope);
                    return TOML_EDIT_ERR;
                }
            } else if (!excluded) {
                toml_assignment_t assignment;
                if (toml_parse_assignment(existing, &line, &assignment) != TOML_EDIT_OK) {
                    toml_header_dispose(&header);
                    toml_key_path_dispose(&scope);
                    return TOML_EDIT_ERR;
                }
                if (assignment.present) {
                    toml_key_path_t full_key;
                    if (toml_key_path_join(&scope, &assignment.key, &full_key) != TOML_EDIT_OK) {
                        toml_assignment_dispose(&assignment);
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&scope);
                        return TOML_EDIT_ERR;
                    }
                    int conflict =
                        !inside_compatible_array && (toml_key_path_has_prefix(&full_key, desired) ||
                                                     toml_key_path_has_prefix(desired, &full_key));
                    toml_key_path_dispose(&full_key);
                    toml_assignment_dispose(&assignment);
                    if (conflict) {
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&scope);
                        return TOML_EDIT_ERR;
                    }
                }
            }
            toml_header_dispose(&header);
        }
        if (toml_scan_line_strings(existing, &line, &multiline_state) != TOML_EDIT_OK) {
            toml_key_path_dispose(&scope);
            return TOML_EDIT_ERR;
        }
    }
    toml_key_path_dispose(&scope);
    return multiline_state == TOML_STRING_NONE ? TOML_EDIT_OK : TOML_EDIT_ERR;
}

static int toml_managed_block_conflicts(const char *existing, size_t existing_len,
                                        size_t exclude_start, size_t exclude_end, const char *block,
                                        size_t block_len) {
    size_t cursor = 0U;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    while (toml_next_line(block, block_len, &cursor, &line)) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        if (!line_in_multiline) {
            toml_header_t header;
            if (toml_parse_header(block, &line, "", &header) != TOML_EDIT_OK) {
                return TOML_EDIT_ERR;
            }
            if (header.present &&
                (toml_block_has_prior_table(block, block_len, line.start, &header.path,
                                            header.array) != TOML_EDIT_OK ||
                 toml_existing_conflicts_with_table(existing, existing_len, exclude_start,
                                                    exclude_end, &header.path,
                                                    header.array) != TOML_EDIT_OK)) {
                toml_header_dispose(&header);
                return TOML_EDIT_ERR;
            }
            toml_header_dispose(&header);
        }
        if (toml_scan_line_strings(block, &line, &multiline_state) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
    }
    return multiline_state == TOML_STRING_NONE ? TOML_EDIT_OK : TOML_EDIT_ERR;
}

static int toml_finish_target_table(toml_target_table_t *current, toml_table_scan_t *result) {
    if (!current->active) {
        return TOML_EDIT_OK;
    }
    if (current->identity_count != 1) {
        return TOML_EDIT_ERR;
    }
    if (current->identity_matches) {
        ++result->matching_count;
        if (result->matching_count > 1) {
            return TOML_EDIT_ERR;
        }
        result->start = current->start;
        result->header_end = current->header_end;
        result->direct_end = current->direct_end;
        result->edit_end = current->last_significant_end;
    }
    memset(current, 0, sizeof(*current));
    return TOML_EDIT_OK;
}

static int toml_scan_named_tables(const char *data, size_t len, const char *table_name,
                                  const char *identity_key, const char *identity_value,
                                  toml_table_scan_t *result) {
    memset(result, 0, sizeof(*result));
    toml_key_path_t root_path;
    if (toml_parse_key_path(table_name, 0U, strlen(table_name), &root_path) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    toml_target_table_t current = {0};
    size_t cursor = 0;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    int at_root = 1;
    while (toml_next_line(data, len, &cursor, &line)) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        int handled_header = 0;
        if (!line_in_multiline) {
            toml_header_t header;
            if (toml_parse_header(data, &line, table_name, &header) != TOML_EDIT_OK) {
                toml_key_path_dispose(&root_path);
                return TOML_EDIT_ERR;
            }
            if (header.present) {
                handled_header = 1;
                at_root = 0;
                int exact_root = toml_key_path_equal(&header.path, &root_path);
                int descendant = header.path.count > root_path.count &&
                                 toml_key_path_has_prefix(&header.path, &root_path);
                if (exact_root) {
                    if (!header.array) {
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&root_path);
                        return TOML_EDIT_ERR;
                    }
                    if (current.active && current.direct_end == 0U) {
                        current.direct_end = current.direct_significant_end;
                    }
                    if (toml_finish_target_table(&current, result) != TOML_EDIT_OK) {
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&root_path);
                        return TOML_EDIT_ERR;
                    }
                    current.active = 1;
                    current.start = header.edit_start;
                    current.header_end = line.full_end;
                    current.direct_significant_end = line.full_end;
                    current.last_significant_end = line.full_end;
                } else if (descendant) {
                    if (!current.active) {
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&root_path);
                        return TOML_EDIT_ERR;
                    }
                    if (!current.descendants) {
                        current.direct_end = current.direct_significant_end;
                        current.descendants = 1;
                    }
                    current.last_significant_end = line.full_end;
                } else {
                    if (current.active && current.direct_end == 0U) {
                        current.direct_end = current.direct_significant_end;
                    }
                    if (toml_finish_target_table(&current, result) != TOML_EDIT_OK) {
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&root_path);
                        return TOML_EDIT_ERR;
                    }
                }
            }
            toml_header_dispose(&header);
        }

        if (!handled_header && current.active) {
            int significant = !toml_line_is_blank_or_comment(data, &line);
            if (significant) {
                current.last_significant_end = line.full_end;
                if (!current.descendants) {
                    current.direct_significant_end = line.full_end;
                }
            }
            if (!line_in_multiline) {
                toml_assignment_t assignment;
                if (toml_parse_assignment(data, &line, &assignment) != TOML_EDIT_OK) {
                    toml_key_path_dispose(&root_path);
                    return TOML_EDIT_ERR;
                }
                if (significant && !assignment.present) {
                    toml_assignment_dispose(&assignment);
                    toml_key_path_dispose(&root_path);
                    return TOML_EDIT_ERR;
                }
                if (!current.descendants && assignment.present &&
                    toml_key_path_is_single(&assignment.key, identity_key)) {
                    int identity_matches = 0;
                    if (toml_assignment_string_equals(data, &assignment, identity_value,
                                                      &identity_matches) != TOML_EDIT_OK) {
                        toml_assignment_dispose(&assignment);
                        toml_key_path_dispose(&root_path);
                        return TOML_EDIT_ERR;
                    }
                    current.identity_count++;
                    if (current.identity_count > 1) {
                        toml_assignment_dispose(&assignment);
                        toml_key_path_dispose(&root_path);
                        return TOML_EDIT_ERR;
                    }
                    current.identity_matches = identity_matches;
                }
                toml_assignment_dispose(&assignment);
            }
        } else if (!handled_header && !current.active && at_root && !line_in_multiline) {
            toml_assignment_t assignment;
            if (toml_parse_assignment(data, &line, &assignment) != TOML_EDIT_OK) {
                toml_key_path_dispose(&root_path);
                return TOML_EDIT_ERR;
            }
            if (assignment.present && (toml_key_path_has_prefix(&assignment.key, &root_path) ||
                                       toml_key_path_has_prefix(&root_path, &assignment.key))) {
                toml_assignment_dispose(&assignment);
                toml_key_path_dispose(&root_path);
                return TOML_EDIT_ERR;
            }
            toml_assignment_dispose(&assignment);
        }
        if (toml_scan_line_strings(data, &line, &multiline_state) != TOML_EDIT_OK) {
            toml_key_path_dispose(&root_path);
            return TOML_EDIT_ERR;
        }
    }
    if (multiline_state != TOML_STRING_NONE) {
        toml_key_path_dispose(&root_path);
        return TOML_EDIT_ERR;
    }
    if (current.active && current.direct_end == 0U) {
        current.direct_end = current.direct_significant_end;
    }
    int finish = toml_finish_target_table(&current, result);
    toml_key_path_dispose(&root_path);
    return finish;
}

static void toml_body_spec_dispose(toml_body_spec_t *spec) {
    if (!spec) {
        return;
    }
    for (size_t i = 0U; i < spec->count; ++i) {
        toml_key_path_dispose(&spec->entries[i].key);
    }
    free(spec->entries);
    memset(spec, 0, sizeof(*spec));
}

static int toml_body_spec_add(toml_body_spec_t *spec, toml_assignment_t *assignment,
                              const toml_line_t *line) {
    for (size_t i = 0U; i < spec->count; ++i) {
        if (toml_key_path_equal(&spec->entries[i].key, &assignment->key)) {
            return TOML_EDIT_ERR;
        }
    }
    if (spec->count == spec->capacity) {
        size_t capacity = spec->capacity ? spec->capacity * 2U : 8U;
        if (capacity < spec->count || capacity > SIZE_MAX / sizeof(*spec->entries)) {
            return TOML_EDIT_ERR;
        }
        toml_body_entry_t *grown =
            (toml_body_entry_t *)realloc(spec->entries, capacity * sizeof(*spec->entries));
        if (!grown) {
            return TOML_EDIT_ERR;
        }
        spec->entries = grown;
        spec->capacity = capacity;
    }
    spec->entries[spec->count].key = assignment->key;
    spec->entries[spec->count].line = *line;
    memset(&assignment->key, 0, sizeof(assignment->key));
    spec->count++;
    return TOML_EDIT_OK;
}

static int toml_validate_table_body(const char *body, size_t body_len, const char *identity_key,
                                    const char *identity_value, toml_body_spec_t *spec) {
    memset(spec, 0, sizeof(*spec));
    if (toml_validate_lexical_strings(body, body_len) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    int identity_count = 0;
    size_t cursor = 0U;
    toml_line_t line;
    while (toml_next_line(body, body_len, &cursor, &line)) {
        toml_header_t header;
        if (toml_parse_header(body, &line, "", &header) != TOML_EDIT_OK) {
            toml_body_spec_dispose(spec);
            return TOML_EDIT_ERR;
        }
        if (header.present) {
            toml_header_dispose(&header);
            toml_body_spec_dispose(spec);
            return TOML_EDIT_ERR;
        }
        toml_header_dispose(&header);
        if (toml_line_is_blank_or_comment(body, &line)) {
            continue;
        }
        toml_assignment_t assignment;
        if (toml_parse_assignment(body, &line, &assignment) != TOML_EDIT_OK ||
            !assignment.present || assignment.multiline_value || assignment.key.count != 1U) {
            toml_assignment_dispose(&assignment);
            toml_body_spec_dispose(spec);
            return TOML_EDIT_ERR;
        }
        if (toml_key_path_is_single(&assignment.key, identity_key)) {
            int matches = 0;
            if (toml_assignment_string_equals(body, &assignment, identity_value, &matches) !=
                    TOML_EDIT_OK ||
                !matches) {
                toml_assignment_dispose(&assignment);
                toml_body_spec_dispose(spec);
                return TOML_EDIT_ERR;
            }
            identity_count++;
        }
        if (toml_body_spec_add(spec, &assignment, &line) != TOML_EDIT_OK) {
            toml_assignment_dispose(&assignment);
            toml_body_spec_dispose(spec);
            return TOML_EDIT_ERR;
        }
        toml_assignment_dispose(&assignment);
    }
    if (identity_count != 1 || spec->count == 0U) {
        toml_body_spec_dispose(spec);
        return TOML_EDIT_ERR;
    }
    return TOML_EDIT_OK;
}

static size_t toml_body_spec_find(const toml_body_spec_t *spec, const toml_key_path_t *key) {
    for (size_t i = 0U; i < spec->count; ++i) {
        if (toml_key_path_equal(&spec->entries[i].key, key)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int toml_append_body_entry(toml_buffer_t *output, const char *body,
                                  const toml_body_entry_t *entry, const char *newline) {
    return toml_buffer_append(output, body + entry->line.start,
                              entry->line.content_end - entry->line.start) == TOML_EDIT_OK &&
                   toml_buffer_append_cstr(output, newline) == TOML_EDIT_OK
               ? TOML_EDIT_OK
               : TOML_EDIT_ERR;
}

static int toml_merge_named_table(const char *existing, size_t existing_len,
                                  const toml_table_scan_t *scan, const char *body,
                                  const toml_body_spec_t *spec, const char *newline,
                                  toml_buffer_t *output) {
    if (toml_buffer_append(output, existing, scan->header_end) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    unsigned char *emitted = (unsigned char *)calloc(spec->count, 1U);
    if (!emitted) {
        return TOML_EDIT_ERR;
    }
    size_t cursor = scan->header_end;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    while (cursor < scan->direct_end && toml_next_line(existing, existing_len, &cursor, &line)) {
        if (line.start >= scan->direct_end) {
            break;
        }
        int replaced = 0;
        if (multiline_state == TOML_STRING_NONE) {
            toml_assignment_t assignment;
            if (toml_parse_assignment(existing, &line, &assignment) != TOML_EDIT_OK) {
                free(emitted);
                return TOML_EDIT_ERR;
            }
            if (assignment.present) {
                size_t desired = toml_body_spec_find(spec, &assignment.key);
                if (desired != SIZE_MAX) {
                    if (emitted[desired] || assignment.multiline_value ||
                        toml_append_body_entry(output, body, &spec->entries[desired], newline) !=
                            TOML_EDIT_OK) {
                        toml_assignment_dispose(&assignment);
                        free(emitted);
                        return TOML_EDIT_ERR;
                    }
                    emitted[desired] = 1U;
                    replaced = 1;
                }
            }
            toml_assignment_dispose(&assignment);
        }
        if (!replaced && toml_buffer_append(output, existing + line.start,
                                            line.full_end - line.start) != TOML_EDIT_OK) {
            free(emitted);
            return TOML_EDIT_ERR;
        }
        if (toml_scan_line_strings(existing, &line, &multiline_state) != TOML_EDIT_OK) {
            free(emitted);
            return TOML_EDIT_ERR;
        }
    }
    if (multiline_state != TOML_STRING_NONE) {
        free(emitted);
        return TOML_EDIT_ERR;
    }
    for (size_t i = 0U; i < spec->count; ++i) {
        if (emitted[i]) {
            continue;
        }
        if (output->len != 0U && output->data[output->len - 1U] != '\n' &&
            toml_buffer_append_cstr(output, newline) != TOML_EDIT_OK) {
            free(emitted);
            return TOML_EDIT_ERR;
        }
        if (toml_append_body_entry(output, body, &spec->entries[i], newline) != TOML_EDIT_OK) {
            free(emitted);
            return TOML_EDIT_ERR;
        }
    }
    free(emitted);
    return toml_buffer_append(output, existing + scan->direct_end, existing_len - scan->direct_end);
}

static int toml_append_named_table(toml_buffer_t *output, const char *table_name,
                                   const char *table_body, const char *newline) {
    size_t body_len = strlen(table_body);
    if (toml_buffer_append_cstr(output, "[[") != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, table_name) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, "]]") != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, newline) != TOML_EDIT_OK ||
        toml_append_normalized_text(output, table_body, body_len, newline) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    return body_len == 0 || table_body[body_len - 1] == '\n' ||
                   toml_buffer_append_cstr(output, newline) == TOML_EDIT_OK
               ? TOML_EDIT_OK
               : TOML_EDIT_ERR;
}

static int toml_named_inputs_valid(const char *file_path, const char *table_name,
                                   const char *identity_key, const char *identity_value) {
    size_t identity_len = 0U;
    return toml_valid_path(file_path) && toml_valid_identifier(table_name) &&
           toml_valid_identifier(identity_key) && identity_value &&
           toml_bounded_length(identity_value, 4096U, &identity_len) == TOML_EDIT_OK &&
           toml_text_is_safe(identity_value, identity_len, 0);
}

int cbm_toml_upsert_named_array_table(const char *file_path, const char *table_name,
                                      const char *identity_key, const char *identity_value,
                                      const char *table_body) {
    size_t body_len = 0U;
    if (!toml_named_inputs_valid(file_path, table_name, identity_key, identity_value) ||
        !table_body ||
        toml_bounded_length(table_body, TOML_EDIT_MAX_BYTES, &body_len) != TOML_EDIT_OK ||
        !toml_text_is_safe(table_body, body_len, 1)) {
        return TOML_EDIT_ERR;
    }
    toml_body_spec_t spec;
    if (toml_validate_table_body(table_body, body_len, identity_key, identity_value, &spec) !=
        TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    char *existing = NULL;
    size_t existing_len = 0;
    toml_file_snapshot_t snapshot;
    if (toml_read_file(file_path, &existing, &existing_len, &snapshot) != TOML_EDIT_OK ||
        !toml_text_is_safe(existing, existing_len, 1)) {
        toml_body_spec_dispose(&spec);
        free(existing);
        return TOML_EDIT_ERR;
    }
    toml_table_scan_t scan;
    if (toml_scan_named_tables(existing, existing_len, table_name, identity_key, identity_value,
                               &scan) != TOML_EDIT_OK) {
        toml_body_spec_dispose(&spec);
        free(existing);
        return TOML_EDIT_ERR;
    }

    toml_buffer_t output = {0};
    const char *newline = toml_newline_style(existing, existing_len);
    int edit_result = TOML_EDIT_OK;
    if (scan.matching_count == 1) {
        edit_result = toml_merge_named_table(existing, existing_len, &scan, table_body, &spec,
                                             newline, &output);
    } else {
        size_t payload_start = existing_len >= 3U && (unsigned char)existing[0] == 0xefU &&
                                       (unsigned char)existing[1] == 0xbbU &&
                                       (unsigned char)existing[2] == 0xbfU
                                   ? 3U
                                   : 0U;
        edit_result = toml_buffer_append(&output, existing, existing_len);
        if (edit_result == TOML_EDIT_OK && existing_len > payload_start) {
            if (existing[existing_len - 1U] != '\n') {
                edit_result = toml_buffer_append_cstr(&output, newline);
            }
            if (edit_result == TOML_EDIT_OK &&
                (output.len < strlen(newline) * 2U ||
                 memcmp(output.data + output.len - strlen(newline) * 2U, newline,
                        strlen(newline)) != 0)) {
                edit_result = toml_buffer_append_cstr(&output, newline);
            }
        }
        if (edit_result == TOML_EDIT_OK) {
            edit_result = toml_append_named_table(&output, table_name, table_body, newline);
        }
    }
    if (edit_result != TOML_EDIT_OK) {
        toml_buffer_dispose(&output);
        toml_body_spec_dispose(&spec);
        free(existing);
        return TOML_EDIT_ERR;
    }
    int result =
        toml_write_atomic(file_path, existing, existing_len, output.data, output.len, &snapshot);
    toml_buffer_dispose(&output);
    toml_body_spec_dispose(&spec);
    free(existing);
    return result;
}

int cbm_toml_remove_named_array_table(const char *file_path, const char *table_name,
                                      const char *identity_key, const char *identity_value) {
    if (!toml_named_inputs_valid(file_path, table_name, identity_key, identity_value)) {
        return TOML_EDIT_ERR;
    }
    char *existing = NULL;
    size_t existing_len = 0;
    toml_file_snapshot_t snapshot;
    if (toml_read_file(file_path, &existing, &existing_len, &snapshot) != TOML_EDIT_OK ||
        !toml_text_is_safe(existing, existing_len, 1)) {
        free(existing);
        return TOML_EDIT_ERR;
    }
    toml_table_scan_t scan;
    if (toml_scan_named_tables(existing, existing_len, table_name, identity_key, identity_value,
                               &scan) != TOML_EDIT_OK) {
        free(existing);
        return TOML_EDIT_ERR;
    }
    if (scan.matching_count == 0) {
        free(existing);
        return TOML_EDIT_OK;
    }

    toml_buffer_t output = {0};
    if (toml_buffer_append(&output, existing, scan.start) != TOML_EDIT_OK ||
        toml_buffer_append(&output, existing + scan.edit_end, existing_len - scan.edit_end) !=
            TOML_EDIT_OK) {
        toml_buffer_dispose(&output);
        free(existing);
        return TOML_EDIT_ERR;
    }
    int result =
        toml_write_atomic(file_path, existing, existing_len, output.data, output.len, &snapshot);
    toml_buffer_dispose(&output);
    free(existing);
    return result;
}

static int toml_owned_table_is_canonical(const char *existing, size_t existing_len,
                                         const toml_table_scan_t *scan, const char *table_name,
                                         const char *canonical_body, const char *newline,
                                         int *is_canonical) {
    *is_canonical = 0;
    if (scan->matching_count != 1 || scan->start > scan->direct_end ||
        scan->direct_end > scan->edit_end || scan->edit_end > existing_len) {
        return TOML_EDIT_ERR;
    }
    if (scan->direct_end != scan->edit_end) {
        return TOML_EDIT_OK;
    }

    toml_buffer_t canonical = {0};
    if (toml_append_named_table(&canonical, table_name, canonical_body, newline) != TOML_EDIT_OK) {
        toml_buffer_dispose(&canonical);
        return TOML_EDIT_ERR;
    }
    size_t existing_table_len = scan->direct_end - scan->start;
    *is_canonical = existing_table_len == canonical.len &&
                    memcmp(existing + scan->start, canonical.data, canonical.len) == 0;
    toml_buffer_dispose(&canonical);
    return TOML_EDIT_OK;
}

static int toml_build_owned_table_insert(toml_buffer_t *output, const char *existing,
                                         size_t existing_len, const char *table_name,
                                         const char *canonical_body, const char *newline) {
    size_t payload_start = existing_len >= 3U && (unsigned char)existing[0] == 0xefU &&
                                   (unsigned char)existing[1] == 0xbbU &&
                                   (unsigned char)existing[2] == 0xbfU
                               ? 3U
                               : 0U;
    int result = toml_buffer_append(output, existing, existing_len);
    if (result == TOML_EDIT_OK && existing_len > payload_start &&
        existing[existing_len - 1U] != '\n') {
        result = toml_buffer_append_cstr(output, newline);
    }
    size_t newline_len = strlen(newline);
    if (result == TOML_EDIT_OK && existing_len > payload_start &&
        (output->len < newline_len * 2U ||
         memcmp(output->data + output->len - newline_len * 2U, newline, newline_len) != 0)) {
        result = toml_buffer_append_cstr(output, newline);
    }
    return result == TOML_EDIT_OK
               ? toml_append_named_table(output, table_name, canonical_body, newline)
               : TOML_EDIT_ERR;
}

static int toml_edit_owned_named_array_table(const char *file_path, const char *table_name,
                                             const char *identity_key, const char *identity_value,
                                             const char *canonical_body, int remove) {
    size_t body_len = 0U;
    if (!toml_named_inputs_valid(file_path, table_name, identity_key, identity_value) ||
        !canonical_body ||
        toml_bounded_length(canonical_body, TOML_EDIT_MAX_BYTES, &body_len) != TOML_EDIT_OK ||
        !toml_text_is_safe(canonical_body, body_len, 1)) {
        return CBM_TOML_OWNED_EDIT_ERROR;
    }
    toml_body_spec_t spec;
    if (toml_validate_table_body(canonical_body, body_len, identity_key, identity_value, &spec) !=
        TOML_EDIT_OK) {
        return CBM_TOML_OWNED_EDIT_ERROR;
    }
    toml_body_spec_dispose(&spec);

    char *existing = NULL;
    size_t existing_len = 0U;
    toml_file_snapshot_t snapshot;
    if (toml_read_file(file_path, &existing, &existing_len, &snapshot) != TOML_EDIT_OK ||
        !toml_text_is_safe(existing, existing_len, 1)) {
        free(existing);
        return CBM_TOML_OWNED_EDIT_ERROR;
    }
    toml_table_scan_t scan;
    if (toml_scan_named_tables(existing, existing_len, table_name, identity_key, identity_value,
                               &scan) != TOML_EDIT_OK) {
        free(existing);
        return CBM_TOML_OWNED_EDIT_ERROR;
    }

    const char *newline = toml_newline_style(existing, existing_len);
    if (scan.matching_count == 1) {
        int is_canonical = 0;
        if (toml_owned_table_is_canonical(existing, existing_len, &scan, table_name, canonical_body,
                                          newline, &is_canonical) != TOML_EDIT_OK) {
            free(existing);
            return CBM_TOML_OWNED_EDIT_ERROR;
        }
        if (!is_canonical) {
            free(existing);
            return CBM_TOML_OWNED_EDIT_FOREIGN;
        }
        if (!remove) {
            free(existing);
            return CBM_TOML_OWNED_EDIT_OK;
        }
    } else if (remove) {
        free(existing);
        return CBM_TOML_OWNED_EDIT_OK;
    }

    toml_buffer_t output = {0};
    int build_result =
        scan.matching_count == 1
            ? (toml_buffer_append(&output, existing, scan.start) == TOML_EDIT_OK &&
                       toml_buffer_append(&output, existing + scan.edit_end,
                                          existing_len - scan.edit_end) == TOML_EDIT_OK
                   ? TOML_EDIT_OK
                   : TOML_EDIT_ERR)
            : toml_build_owned_table_insert(&output, existing, existing_len, table_name,
                                            canonical_body, newline);
    int result = build_result == TOML_EDIT_OK
                     ? toml_write_atomic(file_path, existing, existing_len, output.data, output.len,
                                         &snapshot)
                     : TOML_EDIT_ERR;
    toml_buffer_dispose(&output);
    free(existing);
    return result == TOML_EDIT_OK ? CBM_TOML_OWNED_EDIT_OK : CBM_TOML_OWNED_EDIT_ERROR;
}

int cbm_toml_upsert_owned_named_array_table(const char *file_path, const char *table_name,
                                            const char *identity_key, const char *identity_value,
                                            const char *canonical_body) {
    return toml_edit_owned_named_array_table(file_path, table_name, identity_key, identity_value,
                                             canonical_body, 0);
}

int cbm_toml_remove_owned_named_array_table(const char *file_path, const char *table_name,
                                            const char *identity_key, const char *identity_value,
                                            const char *canonical_body) {
    return toml_edit_owned_named_array_table(file_path, table_name, identity_key, identity_value,
                                             canonical_body, 1);
}

static int toml_legacy_command_is_owned(const char *data, const toml_assignment_t *assignment,
                                        int *owned) {
    *owned = 0;
    if (!assignment->present || assignment->multiline_value) {
        return TOML_EDIT_ERR;
    }
    toml_string_t value = {0};
    size_t value_len = assignment->value_end - assignment->value_start;
    if (toml_parse_string(data + assignment->value_start, value_len, &value) != TOML_EDIT_OK ||
        value.consumed != value_len) {
        toml_string_dispose(&value);
        return TOML_EDIT_ERR;
    }
    size_t basename_start = 0U;
    for (size_t i = 0U; i < value.len; ++i) {
        if (value.data[i] == '/' || value.data[i] == '\\') {
            basename_start = i + 1U;
        }
    }
    static const char binary_name[] = "codebase-memory-mcp";
    static const char windows_binary_name[] = "codebase-memory-mcp.exe";
    size_t basename_len = value.len - basename_start;
    *owned = (basename_len == sizeof(binary_name) - 1U &&
              memcmp(value.data + basename_start, binary_name, basename_len) == 0) ||
             (basename_len == sizeof(windows_binary_name) - 1U &&
              memcmp(value.data + basename_start, windows_binary_name, basename_len) == 0);
    toml_string_dispose(&value);
    return TOML_EDIT_OK;
}

static int toml_legacy_args_are_empty(const char *data, const toml_assignment_t *assignment) {
    if (!assignment->present || assignment->multiline_value) {
        return 0;
    }
    size_t pos = assignment->value_start;
    size_t end = assignment->value_end;
    if (pos >= end || data[pos++] != '[') {
        return 0;
    }
    while (pos < end && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
    }
    if (pos >= end || data[pos++] != ']') {
        return 0;
    }
    while (pos < end && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
    }
    return pos == end;
}

static int toml_legacy_schema_is_owned(int command_count, int command_owned, int args_count,
                                       int args_empty) {
    return command_count == 1 && command_owned && args_count <= 1 &&
           (args_count == 0 || args_empty);
}

int cbm_toml_remove_legacy_table(const char *file_path, const char *table_name,
                                 const char *begin_marker, const char *end_marker) {
    if (!toml_valid_path(file_path) || !table_name || !toml_valid_marker(begin_marker) ||
        !toml_valid_marker(end_marker) || strcmp(begin_marker, end_marker) == 0) {
        return TOML_EDIT_ERR;
    }
    toml_key_path_t desired;
    if (toml_parse_key_path(table_name, 0U, strlen(table_name), &desired) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }

    char *existing = NULL;
    size_t existing_len = 0U;
    toml_file_snapshot_t snapshot;
    if (toml_read_file(file_path, &existing, &existing_len, &snapshot) != TOML_EDIT_OK ||
        !toml_text_is_safe(existing, existing_len, 1)) {
        toml_key_path_dispose(&desired);
        free(existing);
        return TOML_EDIT_ERR;
    }

    toml_line_t begin_line = {0};
    toml_line_t end_line = {0};
    int has_managed_pair = 0;
    if (toml_find_markers(existing, existing_len, begin_marker, end_marker, &begin_line, &end_line,
                          &has_managed_pair) != TOML_EDIT_OK) {
        toml_key_path_dispose(&desired);
        free(existing);
        return TOML_EDIT_ERR;
    }
    if (has_managed_pair) {
        toml_key_path_dispose(&desired);
        free(existing);
        return TOML_EDIT_OK;
    }

    size_t cursor = 0U;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    int target_active = 0;
    int target_count = 0;
    int target_foreign = 0;
    int target_array_seen = 0;
    int target_regular_seen = 0;
    int command_count = 0;
    int command_owned = 0;
    int args_count = 0;
    int args_empty = 0;
    size_t edit_start = SIZE_MAX;
    size_t edit_end = existing_len;
    while (toml_next_line(existing, existing_len, &cursor, &line)) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        int handled_header = 0;
        if (!line_in_multiline) {
            toml_header_t header;
            if (toml_parse_header(existing, &line, "", &header) != TOML_EDIT_OK) {
                toml_key_path_dispose(&desired);
                free(existing);
                return TOML_EDIT_ERR;
            }
            if (header.present) {
                handled_header = 1;
                int exact = toml_key_path_equal(&header.path, &desired);
                int descendant = header.path.count > desired.count &&
                                 toml_key_path_has_prefix(&header.path, &desired);
                if (exact) {
                    if (header.array) {
                        if (target_regular_seen) {
                            toml_header_dispose(&header);
                            toml_key_path_dispose(&desired);
                            free(existing);
                            return TOML_EDIT_ERR;
                        }
                        target_array_seen = 1;
                        target_foreign = 1;
                        target_count++;
                    } else if (target_array_seen || target_regular_seen) {
                        toml_header_dispose(&header);
                        toml_key_path_dispose(&desired);
                        free(existing);
                        return TOML_EDIT_ERR;
                    } else {
                        target_regular_seen = 1;
                        target_count = 1;
                    }
                    target_active = 1;
                    command_count = 0;
                    command_owned = 0;
                    args_count = 0;
                    args_empty = 0;
                    edit_start = header.edit_start;
                } else if (target_active) {
                    if (descendant || !toml_legacy_schema_is_owned(command_count, command_owned,
                                                                   args_count, args_empty)) {
                        target_foreign = 1;
                    }
                    edit_end = header.edit_start;
                    target_active = 0;
                }
            }
            toml_header_dispose(&header);
        }
        if (target_active && !handled_header && !line_in_multiline &&
            !toml_line_is_blank_or_comment(existing, &line)) {
            toml_assignment_t assignment;
            if (toml_parse_assignment(existing, &line, &assignment) != TOML_EDIT_OK ||
                !assignment.present) {
                toml_assignment_dispose(&assignment);
                toml_key_path_dispose(&desired);
                free(existing);
                return TOML_EDIT_ERR;
            }
            if (assignment.key.count != 1U) {
                target_foreign = 1;
            } else if (toml_key_path_is_single(&assignment.key, "command")) {
                if (++command_count > 1 ||
                    toml_legacy_command_is_owned(existing, &assignment, &command_owned) !=
                        TOML_EDIT_OK) {
                    toml_assignment_dispose(&assignment);
                    toml_key_path_dispose(&desired);
                    free(existing);
                    return TOML_EDIT_ERR;
                }
                if (!command_owned) {
                    target_foreign = 1;
                }
            } else if (toml_key_path_is_single(&assignment.key, "args")) {
                args_count++;
                args_empty = toml_legacy_args_are_empty(existing, &assignment);
                if (args_count > 1) {
                    toml_assignment_dispose(&assignment);
                    toml_key_path_dispose(&desired);
                    free(existing);
                    return TOML_EDIT_ERR;
                }
                if (!args_empty) {
                    target_foreign = 1;
                }
            } else {
                target_foreign = 1;
            }
            toml_assignment_dispose(&assignment);
        }
        if (toml_scan_line_strings(existing, &line, &multiline_state) != TOML_EDIT_OK) {
            toml_key_path_dispose(&desired);
            free(existing);
            return TOML_EDIT_ERR;
        }
    }
    toml_key_path_dispose(&desired);
    if (multiline_state != TOML_STRING_NONE) {
        free(existing);
        return TOML_EDIT_ERR;
    }
    if (target_active &&
        !toml_legacy_schema_is_owned(command_count, command_owned, args_count, args_empty)) {
        target_foreign = 1;
    }
    if (target_foreign) {
        free(existing);
        return TOML_EDIT_FOREIGN;
    }
    if (target_count == 0) {
        free(existing);
        return TOML_EDIT_OK;
    }

    toml_buffer_t output = {0};
    if (toml_buffer_append(&output, existing, edit_start) != TOML_EDIT_OK ||
        toml_buffer_append(&output, existing + edit_end, existing_len - edit_end) != TOML_EDIT_OK) {
        toml_buffer_dispose(&output);
        free(existing);
        return TOML_EDIT_ERR;
    }
    int result =
        toml_write_atomic(file_path, existing, existing_len, output.data, output.len, &snapshot);
    toml_buffer_dispose(&output);
    free(existing);
    return result;
}

enum {
    TOML_CODEX_EVENT_NONE = 0,
    TOML_CODEX_EVENT_SESSION = 1,
    TOML_CODEX_EVENT_SUBAGENT = 2,
    TOML_CODEX_VALUE_MAX_DEPTH = 32,
    TOML_CODEX_MAX_ITEMS = 65536,
};

typedef struct {
    size_t start;
    size_t end;
} toml_codex_span_t;

typedef struct {
    toml_codex_span_t *items;
    size_t count;
    size_t capacity;
} toml_codex_span_vector_t;

typedef struct {
    toml_key_path_t key;
    toml_codex_span_t value;
} toml_codex_field_t;

typedef struct {
    toml_codex_field_t *items;
    size_t count;
    size_t capacity;
} toml_codex_field_vector_t;

static void toml_codex_span_vector_dispose(toml_codex_span_vector_t *vector) {
    if (!vector) {
        return;
    }
    free(vector->items);
    memset(vector, 0, sizeof(*vector));
}

static int toml_codex_span_vector_push(toml_codex_span_vector_t *vector, size_t start, size_t end) {
    if (!vector || start > end || vector->count >= TOML_CODEX_MAX_ITEMS) {
        return TOML_EDIT_ERR;
    }
    if (vector->count == vector->capacity) {
        size_t capacity = vector->capacity ? vector->capacity * 2U : 8U;
        if (capacity > TOML_CODEX_MAX_ITEMS || capacity > SIZE_MAX / sizeof(*vector->items)) {
            capacity = TOML_CODEX_MAX_ITEMS;
        }
        if (capacity <= vector->count) {
            return TOML_EDIT_ERR;
        }
        toml_codex_span_t *items =
            (toml_codex_span_t *)realloc(vector->items, capacity * sizeof(*items));
        if (!items) {
            return TOML_EDIT_ERR;
        }
        vector->items = items;
        vector->capacity = capacity;
    }
    vector->items[vector->count++] = (toml_codex_span_t){.start = start, .end = end};
    return TOML_EDIT_OK;
}

static void toml_codex_field_vector_dispose(toml_codex_field_vector_t *vector) {
    if (!vector) {
        return;
    }
    for (size_t i = 0U; i < vector->count; ++i) {
        toml_key_path_dispose(&vector->items[i].key);
    }
    free(vector->items);
    memset(vector, 0, sizeof(*vector));
}

static int toml_codex_field_vector_push(toml_codex_field_vector_t *vector, toml_key_path_t *key,
                                        size_t value_start, size_t value_end) {
    if (!vector || !key || value_start > value_end || vector->count >= TOML_CODEX_MAX_ITEMS) {
        return TOML_EDIT_ERR;
    }
    for (size_t i = 0U; i < vector->count; ++i) {
        if (toml_key_path_equal(&vector->items[i].key, key)) {
            return TOML_EDIT_ERR;
        }
    }
    if (vector->count == vector->capacity) {
        size_t capacity = vector->capacity ? vector->capacity * 2U : 8U;
        if (capacity > TOML_CODEX_MAX_ITEMS || capacity > SIZE_MAX / sizeof(*vector->items)) {
            capacity = TOML_CODEX_MAX_ITEMS;
        }
        if (capacity <= vector->count) {
            return TOML_EDIT_ERR;
        }
        toml_codex_field_t *items =
            (toml_codex_field_t *)realloc(vector->items, capacity * sizeof(*items));
        if (!items) {
            return TOML_EDIT_ERR;
        }
        vector->items = items;
        vector->capacity = capacity;
    }
    vector->items[vector->count] = (toml_codex_field_t){
        .key = *key,
        .value = {.start = value_start, .end = value_end},
    };
    memset(key, 0, sizeof(*key));
    vector->count++;
    return TOML_EDIT_OK;
}

static int toml_codex_skip_array_space(const char *data, size_t len, size_t *position) {
    size_t pos = *position;
    for (;;) {
        while (pos < len &&
               (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' || data[pos] == '\n')) {
            pos++;
        }
        if (pos >= len || data[pos] != '#') {
            break;
        }
        while (pos < len && data[pos] != '\n') {
            pos++;
        }
    }
    *position = pos;
    return TOML_EDIT_OK;
}

static void toml_codex_skip_inline_space(const char *data, size_t len, size_t *position) {
    while (*position < len && (data[*position] == ' ' || data[*position] == '\t')) {
        (*position)++;
    }
}

static int toml_codex_digit_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int toml_codex_digits_are_valid(const char *data, size_t start, size_t end, int base,
                                       size_t *digit_count, char *first_digit) {
    if (start >= end) {
        return 0;
    }
    size_t count = 0U;
    int previous_was_digit = 0;
    for (size_t pos = start; pos < end; ++pos) {
        int digit = toml_codex_digit_value(data[pos]);
        if (digit >= 0 && digit < base) {
            if (count == 0U && first_digit) {
                *first_digit = data[pos];
            }
            count++;
            previous_was_digit = 1;
        } else if (data[pos] == '_' && previous_was_digit && pos + 1U < end) {
            int next = toml_codex_digit_value(data[pos + 1U]);
            if (next < 0 || next >= base) {
                return 0;
            }
            previous_was_digit = 0;
        } else {
            return 0;
        }
    }
    if (!previous_was_digit) {
        return 0;
    }
    if (digit_count) {
        *digit_count = count;
    }
    return 1;
}

static int toml_codex_fixed_uint(const char *data, size_t start, size_t count, int *value) {
    int parsed = 0;
    for (size_t i = 0U; i < count; ++i) {
        if (data[start + i] < '0' || data[start + i] > '9') {
            return 0;
        }
        parsed = parsed * 10 + (data[start + i] - '0');
    }
    *value = parsed;
    return 1;
}

static int toml_codex_date_is_valid(const char *data, size_t len) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (len < 10U || data[4] != '-' || data[7] != '-' ||
        !toml_codex_fixed_uint(data, 0U, 4U, &year) ||
        !toml_codex_fixed_uint(data, 5U, 2U, &month) ||
        !toml_codex_fixed_uint(data, 8U, 2U, &day) || month < 1 || month > 12) {
        return 0;
    }
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maximum = month_days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        maximum = 29;
    }
    return day >= 1 && day <= maximum;
}

static int toml_codex_time_end(const char *data, size_t len, size_t start, size_t *end_out) {
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (start > len || len - start < 8U || data[start + 2U] != ':' || data[start + 5U] != ':' ||
        !toml_codex_fixed_uint(data, start, 2U, &hour) ||
        !toml_codex_fixed_uint(data, start + 3U, 2U, &minute) ||
        !toml_codex_fixed_uint(data, start + 6U, 2U, &second) || hour > 23 || minute > 59 ||
        second > 60) {
        return 0;
    }
    size_t pos = start + 8U;
    if (pos < len && data[pos] == '.') {
        size_t fraction_start = ++pos;
        while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
            pos++;
        }
        if (pos == fraction_start) {
            return 0;
        }
    }
    *end_out = pos;
    return 1;
}

static int toml_codex_datetime_is_valid(const char *data, size_t len) {
    if (len == 10U) {
        return toml_codex_date_is_valid(data, len);
    }
    if (len >= 8U && data[2] == ':' && data[5] == ':') {
        size_t time_end = 0U;
        return toml_codex_time_end(data, len, 0U, &time_end) && time_end == len;
    }
    if (len < 19U || !toml_codex_date_is_valid(data, 10U) ||
        (data[10] != 'T' && data[10] != 't' && data[10] != ' ')) {
        return 0;
    }
    size_t time_end = 0U;
    if (!toml_codex_time_end(data, len, 11U, &time_end)) {
        return 0;
    }
    if (time_end == len) {
        return 1;
    }
    if ((data[time_end] == 'Z' || data[time_end] == 'z') && time_end + 1U == len) {
        return 1;
    }
    int offset_hour = 0;
    int offset_minute = 0;
    return time_end + 6U == len && (data[time_end] == '+' || data[time_end] == '-') &&
           data[time_end + 3U] == ':' &&
           toml_codex_fixed_uint(data, time_end + 1U, 2U, &offset_hour) &&
           toml_codex_fixed_uint(data, time_end + 4U, 2U, &offset_minute) && offset_hour <= 23 &&
           offset_minute <= 59;
}

static int toml_codex_number_is_valid(const char *data, size_t len) {
    size_t pos = 0U;
    if (pos < len && (data[pos] == '+' || data[pos] == '-')) {
        pos++;
    }
    if (pos == len) {
        return 0;
    }
    if ((len - pos == 3U && memcmp(data + pos, "inf", 3U) == 0) ||
        (len - pos == 3U && memcmp(data + pos, "nan", 3U) == 0)) {
        return 1;
    }
    if (pos == 0U && len >= 3U && data[0] == '0' &&
        (data[1] == 'x' || data[1] == 'o' || data[1] == 'b')) {
        int base = data[1] == 'x' ? 16 : (data[1] == 'o' ? 8 : 2);
        return toml_codex_digits_are_valid(data, 2U, len, base, NULL, NULL);
    }
    size_t integer_start = pos;
    while (pos < len && data[pos] != '.' && data[pos] != 'e' && data[pos] != 'E') {
        pos++;
    }
    size_t digit_count = 0U;
    char first_digit = '\0';
    if (!toml_codex_digits_are_valid(data, integer_start, pos, 10, &digit_count, &first_digit) ||
        (digit_count > 1U && first_digit == '0')) {
        return 0;
    }
    int is_float = 0;
    if (pos < len && data[pos] == '.') {
        is_float = 1;
        size_t fraction_start = ++pos;
        while (pos < len && data[pos] != 'e' && data[pos] != 'E') {
            pos++;
        }
        if (!toml_codex_digits_are_valid(data, fraction_start, pos, 10, NULL, NULL)) {
            return 0;
        }
    }
    if (pos < len && (data[pos] == 'e' || data[pos] == 'E')) {
        is_float = 1;
        pos++;
        if (pos < len && (data[pos] == '+' || data[pos] == '-')) {
            pos++;
        }
        if (!toml_codex_digits_are_valid(data, pos, len, 10, NULL, NULL)) {
            return 0;
        }
        pos = len;
    }
    return pos == len && (is_float || digit_count != 0U);
}

static int toml_codex_primitive_is_valid(const char *data, size_t len) {
    if (!data || len == 0U) {
        return 0;
    }
    if ((len == 4U && memcmp(data, "true", 4U) == 0) ||
        (len == 5U && memcmp(data, "false", 5U) == 0)) {
        return 1;
    }
    if ((len >= 10U && data[4] == '-' && data[7] == '-') ||
        (len >= 8U && data[2] == ':' && data[5] == ':')) {
        return toml_codex_datetime_is_valid(data, len);
    }
    return toml_codex_number_is_valid(data, len);
}

static int toml_codex_parse_value_end(const char *data, size_t len, size_t start, unsigned depth,
                                      size_t *end_out);

static int toml_codex_parse_inline_key(const char *data, size_t len, size_t *position,
                                       toml_key_path_t *key) {
    size_t start = *position;
    size_t pos = start;
    char quote = '\0';
    int escaped = 0;
    while (pos < len) {
        char ch = data[pos];
        if (quote) {
            if (quote == '"' && !escaped && ch == '\\') {
                escaped = 1;
            } else {
                if (!escaped && ch == quote) {
                    quote = '\0';
                }
                escaped = 0;
            }
            pos++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            pos++;
            continue;
        }
        if (ch == '=') {
            break;
        }
        if (ch == '\r' || ch == '\n' || ch == '#' || ch == ',' || ch == '}') {
            return TOML_EDIT_ERR;
        }
        pos++;
    }
    if (quote || pos >= len || data[pos] != '=') {
        return TOML_EDIT_ERR;
    }
    size_t key_end = pos;
    toml_trim(data, &start, &key_end);
    if (start == key_end || toml_parse_key_path(data, start, key_end, key) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    *position = pos + 1U;
    return TOML_EDIT_OK;
}

static int toml_codex_validate_inline_table(const char *data, size_t len, size_t start,
                                            unsigned depth, size_t *end_out) {
    size_t pos = start + 1U;
    toml_codex_skip_inline_space(data, len, &pos);
    if (pos < len && data[pos] == '}') {
        *end_out = pos + 1U;
        return TOML_EDIT_OK;
    }
    for (;;) {
        toml_key_path_t key = {0};
        if (toml_codex_parse_inline_key(data, len, &pos, &key) != TOML_EDIT_OK) {
            toml_key_path_dispose(&key);
            return TOML_EDIT_ERR;
        }
        toml_key_path_dispose(&key);
        toml_codex_skip_inline_space(data, len, &pos);
        size_t value_end = 0U;
        if (toml_codex_parse_value_end(data, len, pos, depth + 1U, &value_end) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
        pos = value_end;
        toml_codex_skip_inline_space(data, len, &pos);
        if (pos >= len) {
            return TOML_EDIT_ERR;
        }
        if (data[pos] == '}') {
            *end_out = pos + 1U;
            return TOML_EDIT_OK;
        }
        if (data[pos] != ',') {
            return TOML_EDIT_ERR;
        }
        pos++;
        toml_codex_skip_inline_space(data, len, &pos);
        if (pos >= len || data[pos] == '}') {
            return TOML_EDIT_ERR;
        }
    }
}

static int toml_codex_validate_array(const char *data, size_t len, size_t start, unsigned depth,
                                     size_t *end_out) {
    size_t pos = start + 1U;
    toml_codex_skip_array_space(data, len, &pos);
    if (pos < len && data[pos] == ']') {
        *end_out = pos + 1U;
        return TOML_EDIT_OK;
    }
    size_t count = 0U;
    for (;;) {
        if (++count > TOML_CODEX_MAX_ITEMS) {
            return TOML_EDIT_ERR;
        }
        size_t value_end = 0U;
        if (toml_codex_parse_value_end(data, len, pos, depth + 1U, &value_end) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
        pos = value_end;
        toml_codex_skip_array_space(data, len, &pos);
        if (pos >= len) {
            return TOML_EDIT_ERR;
        }
        if (data[pos] == ']') {
            *end_out = pos + 1U;
            return TOML_EDIT_OK;
        }
        if (data[pos] != ',') {
            return TOML_EDIT_ERR;
        }
        pos++;
        toml_codex_skip_array_space(data, len, &pos);
        if (pos < len && data[pos] == ']') {
            *end_out = pos + 1U;
            return TOML_EDIT_OK;
        }
    }
}

static int toml_codex_parse_value_end(const char *data, size_t len, size_t start, unsigned depth,
                                      size_t *end_out) {
    if (!data || !end_out || start >= len || depth > TOML_CODEX_VALUE_MAX_DEPTH) {
        return TOML_EDIT_ERR;
    }
    if (data[start] == '"' || data[start] == '\'') {
        toml_string_t value = {0};
        int result = toml_parse_string(data + start, len - start, &value);
        if (result == TOML_EDIT_OK) {
            *end_out = start + value.consumed;
        }
        toml_string_dispose(&value);
        return result;
    }
    if (data[start] == '[') {
        return toml_codex_validate_array(data, len, start, depth, end_out);
    }
    if (data[start] == '{') {
        return toml_codex_validate_inline_table(data, len, start, depth, end_out);
    }
    size_t pos = start;
    while (pos < len && data[pos] != '\t' && data[pos] != '\r' && data[pos] != '\n' &&
           data[pos] != '#' && data[pos] != ',' && data[pos] != ']' && data[pos] != '}') {
        if (data[pos] == ' ' && !(pos == start + 10U && len - start >= 19U &&
                                  data[start + 4U] == '-' && data[start + 7U] == '-')) {
            break;
        }
        pos++;
    }
    if (pos == start || !toml_codex_primitive_is_valid(data + start, pos - start)) {
        return TOML_EDIT_ERR;
    }
    *end_out = pos;
    return TOML_EDIT_OK;
}

static int toml_codex_parse_array_items(const char *data, size_t len, size_t start,
                                        size_t expected_end, toml_codex_span_vector_t *items) {
    if (start >= len || data[start] != '[') {
        return TOML_EDIT_ERR;
    }
    size_t pos = start + 1U;
    toml_codex_skip_array_space(data, len, &pos);
    if (pos < len && data[pos] == ']') {
        return pos + 1U == expected_end ? TOML_EDIT_OK : TOML_EDIT_ERR;
    }
    for (;;) {
        size_t item_start = pos;
        size_t item_end = 0U;
        if (toml_codex_parse_value_end(data, len, pos, 1U, &item_end) != TOML_EDIT_OK ||
            toml_codex_span_vector_push(items, item_start, item_end) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
        pos = item_end;
        toml_codex_skip_array_space(data, len, &pos);
        if (pos >= len) {
            return TOML_EDIT_ERR;
        }
        if (data[pos] == ']') {
            return pos + 1U == expected_end ? TOML_EDIT_OK : TOML_EDIT_ERR;
        }
        if (data[pos] != ',') {
            return TOML_EDIT_ERR;
        }
        pos++;
        toml_codex_skip_array_space(data, len, &pos);
        if (pos < len && data[pos] == ']') {
            return pos + 1U == expected_end ? TOML_EDIT_OK : TOML_EDIT_ERR;
        }
    }
}

static int toml_codex_parse_inline_fields(const char *data, size_t len, size_t start,
                                          size_t expected_end, toml_codex_field_vector_t *fields) {
    if (start >= len || data[start] != '{') {
        return TOML_EDIT_ERR;
    }
    size_t pos = start + 1U;
    toml_codex_skip_inline_space(data, len, &pos);
    if (pos < len && data[pos] == '}') {
        return pos + 1U == expected_end ? TOML_EDIT_OK : TOML_EDIT_ERR;
    }
    for (;;) {
        toml_key_path_t key = {0};
        if (toml_codex_parse_inline_key(data, len, &pos, &key) != TOML_EDIT_OK) {
            toml_key_path_dispose(&key);
            return TOML_EDIT_ERR;
        }
        toml_codex_skip_inline_space(data, len, &pos);
        size_t value_start = pos;
        size_t value_end = 0U;
        if (toml_codex_parse_value_end(data, len, value_start, 1U, &value_end) != TOML_EDIT_OK ||
            toml_codex_field_vector_push(fields, &key, value_start, value_end) != TOML_EDIT_OK) {
            toml_key_path_dispose(&key);
            return TOML_EDIT_ERR;
        }
        pos = value_end;
        toml_codex_skip_inline_space(data, len, &pos);
        if (pos >= len) {
            return TOML_EDIT_ERR;
        }
        if (data[pos] == '}') {
            return pos + 1U == expected_end ? TOML_EDIT_OK : TOML_EDIT_ERR;
        }
        if (data[pos] != ',') {
            return TOML_EDIT_ERR;
        }
        pos++;
        toml_codex_skip_inline_space(data, len, &pos);
        if (pos >= len || data[pos] == '}') {
            return TOML_EDIT_ERR;
        }
    }
}

static const toml_codex_field_t *toml_codex_find_field(const toml_codex_field_vector_t *fields,
                                                       const char *name) {
    if (!fields || !name) {
        return NULL;
    }
    for (size_t i = 0U; i < fields->count; ++i) {
        if (toml_key_path_is_single(&fields->items[i].key, name)) {
            return &fields->items[i];
        }
    }
    return NULL;
}

static int toml_codex_field_string(const char *data, const toml_codex_field_t *field,
                                   toml_string_t *value) {
    if (!data || !field || !value || field->value.start >= field->value.end) {
        return TOML_EDIT_ERR;
    }
    size_t len = field->value.end - field->value.start;
    if (toml_parse_string(data + field->value.start, len, value) != TOML_EDIT_OK ||
        value->consumed != len) {
        toml_string_dispose(value);
        return TOML_EDIT_ERR;
    }
    return TOML_EDIT_OK;
}

static int toml_codex_field_string_equals(const char *data, const toml_codex_field_t *field,
                                          const char *expected) {
    toml_string_t value = {0};
    if (toml_codex_field_string(data, field, &value) != TOML_EDIT_OK) {
        return 0;
    }
    size_t expected_len = strlen(expected);
    int equal = value.len == expected_len && memcmp(value.data, expected, expected_len) == 0;
    toml_string_dispose(&value);
    return equal;
}

static int toml_codex_field_is_timeout(const char *data, const toml_codex_field_t *field) {
    return field && field->value.end - field->value.start == 1U && data[field->value.start] == '5';
}

static int toml_codex_decode_posix_word(const char *encoded, size_t len, toml_buffer_t *decoded) {
    if (!encoded || len < 2U || encoded[0] != '\'') {
        return TOML_EDIT_ERR;
    }
    size_t pos = 1U;
    for (;;) {
        size_t segment_start = pos;
        while (pos < len && encoded[pos] != '\'') {
            unsigned char ch = (unsigned char)encoded[pos];
            if (ch < 0x20U || ch == 0x7fU) {
                return TOML_EDIT_ERR;
            }
            pos++;
        }
        if (pos >= len || toml_buffer_append(decoded, encoded + segment_start,
                                             pos - segment_start) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
        pos++;
        if (pos == len) {
            return TOML_EDIT_OK;
        }
        if (pos + 2U >= len || encoded[pos] != '\\' || encoded[pos + 1U] != '\'' ||
            encoded[pos + 2U] != '\'') {
            return TOML_EDIT_ERR;
        }
        if (toml_buffer_append_char(decoded, '\'') != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
        pos += 3U;
    }
}

static int toml_codex_decode_powershell_word(const char *encoded, size_t len,
                                             toml_buffer_t *decoded) {
    if (!encoded || len < 2U || encoded[0] != '\'' || encoded[len - 1U] != '\'') {
        return TOML_EDIT_ERR;
    }
    size_t pos = 1U;
    while (pos + 1U < len) {
        unsigned char ch = (unsigned char)encoded[pos++];
        if (ch < 0x20U || ch == 0x7fU) {
            return TOML_EDIT_ERR;
        }
        if (ch == '\'') {
            if (pos >= len - 1U || encoded[pos] != '\'') {
                return TOML_EDIT_ERR;
            }
            pos++;
        }
        if (toml_buffer_append_char(decoded, (char)ch) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
    }
    return pos == len - 1U ? TOML_EDIT_OK : TOML_EDIT_ERR;
}

static int toml_codex_decode_executable(const char *encoded, size_t len, toml_buffer_t *decoded) {
    if (!encoded || len == 0U) {
        return TOML_EDIT_ERR;
    }
    if (encoded[0] == '\'') {
        if (toml_codex_decode_posix_word(encoded, len, decoded) == TOML_EDIT_OK) {
            return TOML_EDIT_OK;
        }
        toml_buffer_dispose(decoded);
        return toml_codex_decode_powershell_word(encoded, len, decoded);
    }
    for (size_t i = 0U; i < len; ++i) {
        unsigned char ch = (unsigned char)encoded[i];
        if (ch < 0x21U || ch == 0x7fU || strchr("\"'`$;&|<>(){}[]*?!", (int)ch)) {
            return TOML_EDIT_ERR;
        }
    }
    return toml_buffer_append(decoded, encoded, len);
}

static int toml_codex_current_command_is_owned(const char *command, size_t len) {
    static const char suffix[] = " hook-augment";
    size_t suffix_len = sizeof(suffix) - 1U;
    size_t start = 0U;
    if (len >= 2U && command[0] == '&' && command[1] == ' ') {
        start = 2U;
    }
    if (len <= start + suffix_len || memcmp(command + len - suffix_len, suffix, suffix_len) != 0) {
        return 0;
    }
    size_t executable_len = len - start - suffix_len;
    toml_buffer_t executable = {0};
    if (toml_codex_decode_executable(command + start, executable_len, &executable) !=
            TOML_EDIT_OK ||
        executable.len == 0U) {
        toml_buffer_dispose(&executable);
        return 0;
    }
    size_t basename_start = 0U;
    for (size_t i = 0U; i < executable.len; ++i) {
        if (executable.data[i] == '/' || executable.data[i] == '\\') {
            basename_start = i + 1U;
        }
    }
    static const char binary[] = "codebase-memory-mcp";
    static const char binary_windows[] = "codebase-memory-mcp.exe";
    size_t basename_len = executable.len - basename_start;
    int owned = (basename_len == sizeof(binary) - 1U &&
                 memcmp(executable.data + basename_start, binary, basename_len) == 0) ||
                (basename_len == sizeof(binary_windows) - 1U &&
                 memcmp(executable.data + basename_start, binary_windows, basename_len) == 0);
    toml_buffer_dispose(&executable);
    return owned;
}

static int toml_codex_command_kind(const char *command, size_t len) {
    static const char legacy_short[] = "echo \"Code discovery: prefer codebase-memory-mcp\"";
    static const char legacy_released[] =
        "echo \"Code discovery: prefer codebase-memory-mcp (search_graph, trace_path, "
        "get_code_snippet, query_graph, search_code) over grep/file-read; run index_repository "
        "first if the project is not indexed.\"";
    if ((len == sizeof(legacy_short) - 1U &&
         memcmp(command, legacy_short, sizeof(legacy_short) - 1U) == 0) ||
        (len == sizeof(legacy_released) - 1U &&
         memcmp(command, legacy_released, sizeof(legacy_released) - 1U) == 0)) {
        return 2;
    }
    return toml_codex_current_command_is_owned(command, len) ? 1 : 0;
}

static int toml_codex_hook_fields_kind(const char *data, const toml_codex_field_vector_t *fields) {
    const toml_codex_field_t *type = toml_codex_find_field(fields, "type");
    const toml_codex_field_t *command = toml_codex_find_field(fields, "command");
    const toml_codex_field_t *command_windows = toml_codex_find_field(fields, "command_windows");
    const toml_codex_field_t *timeout = toml_codex_find_field(fields, "timeout");
    if (!type || !command || !toml_codex_field_string_equals(data, type, "command")) {
        return 0;
    }
    toml_string_t command_value = {0};
    if (toml_codex_field_string(data, command, &command_value) != TOML_EDIT_OK) {
        return 0;
    }
    int command_kind = toml_codex_command_kind(command_value.data, command_value.len);
    toml_string_dispose(&command_value);
    if (command_kind == 2) {
        return fields->count == 2U ? 2 : 0;
    }
    if (command_kind != 1 || fields->count != 4U || !command_windows || !timeout ||
        !toml_codex_field_is_timeout(data, timeout)) {
        return 0;
    }
    toml_string_t windows_value = {0};
    if (toml_codex_field_string(data, command_windows, &windows_value) != TOML_EDIT_OK) {
        return 0;
    }
    int windows_owned = toml_codex_current_command_is_owned(windows_value.data, windows_value.len);
    toml_string_dispose(&windows_value);
    return windows_owned ? 1 : 0;
}

typedef struct {
    toml_key_path_t key;
    size_t start;
    size_t full_end;
    size_t value_start;
    size_t value_end;
} toml_codex_assignment_view_t;

typedef struct {
    int found;
    int under_hooks_table;
    size_t start;
    size_t full_end;
    size_t value_start;
    size_t value_end;
} toml_codex_inline_assignment_t;

typedef struct {
    toml_codex_inline_assignment_t session;
    toml_codex_inline_assignment_t subagent;
} toml_codex_inline_scan_t;

static void toml_codex_assignment_view_dispose(toml_codex_assignment_view_t *assignment) {
    if (assignment) {
        toml_key_path_dispose(&assignment->key);
        memset(assignment, 0, sizeof(*assignment));
    }
}

static int toml_codex_assignment_at_line(const char *data, size_t len, const toml_line_t *line,
                                         toml_codex_assignment_view_t *view) {
    memset(view, 0, sizeof(*view));
    toml_assignment_t assignment;
    if (toml_parse_assignment(data, line, &assignment) != TOML_EDIT_OK || !assignment.present ||
        assignment.multiline_value) {
        toml_assignment_dispose(&assignment);
        return TOML_EDIT_ERR;
    }
    size_t value_end = 0U;
    if (toml_codex_parse_value_end(data, len, assignment.value_start, 0U, &value_end) !=
        TOML_EDIT_OK) {
        toml_assignment_dispose(&assignment);
        return TOML_EDIT_ERR;
    }
    size_t pos = value_end;
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
        pos++;
    }
    if (pos < len && data[pos] == '#') {
        while (pos < len && data[pos] != '\n') {
            pos++;
        }
    }
    if (pos < len && data[pos] == '\r') {
        if (pos + 1U >= len || data[pos + 1U] != '\n') {
            toml_assignment_dispose(&assignment);
            return TOML_EDIT_ERR;
        }
        pos += 2U;
    } else if (pos < len && data[pos] == '\n') {
        pos++;
    } else if (pos != len) {
        toml_assignment_dispose(&assignment);
        return TOML_EDIT_ERR;
    }
    view->key = assignment.key;
    memset(&assignment.key, 0, sizeof(assignment.key));
    view->start = line->start;
    view->full_end = pos;
    view->value_start = assignment.value_start;
    view->value_end = value_end;
    toml_assignment_dispose(&assignment);
    return TOML_EDIT_OK;
}

static int toml_codex_event_for_path(const toml_key_path_t *path) {
    if (!path || path->count != 2U || strcmp(toml_key_path_segment(path, 0U), "hooks") != 0) {
        return TOML_CODEX_EVENT_NONE;
    }
    const char *event = toml_key_path_segment(path, 1U);
    if (strcmp(event, "SessionStart") == 0) {
        return TOML_CODEX_EVENT_SESSION;
    }
    if (strcmp(event, "SubagentStart") == 0) {
        return TOML_CODEX_EVENT_SUBAGENT;
    }
    return TOML_CODEX_EVENT_NONE;
}

static int toml_codex_path_is_event_descendant(const toml_key_path_t *path, int event) {
    const char *event_name = event == TOML_CODEX_EVENT_SESSION ? "SessionStart" : "SubagentStart";
    return path && path->count > 2U && strcmp(toml_key_path_segment(path, 0U), "hooks") == 0 &&
           strcmp(toml_key_path_segment(path, 1U), event_name) == 0;
}

static int toml_codex_path_is_hooks_root(const toml_key_path_t *path) {
    return path && path->count == 1U && strcmp(toml_key_path_segment(path, 0U), "hooks") == 0;
}

static int toml_codex_assignment_path_conflicts(const toml_key_path_t *path) {
    if (!path || path->count == 0U || strcmp(toml_key_path_segment(path, 0U), "hooks") != 0) {
        return 0;
    }
    if (path->count == 1U) {
        return 1;
    }
    const char *event = toml_key_path_segment(path, 1U);
    return strcmp(event, "SessionStart") == 0 || strcmp(event, "SubagentStart") == 0;
}

static int toml_codex_scan_inline_assignments(const char *data, size_t len,
                                              toml_codex_inline_scan_t *result) {
    memset(result, 0, sizeof(*result));
    size_t cursor = 0U;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    int scope_is_target_array = 0;
    toml_key_path_t scope = {0};
    while (toml_next_line(data, len, &cursor, &line)) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        int handled = 0;
        if (!line_in_multiline) {
            toml_header_t header;
            if (toml_parse_header(data, &line, "", &header) != TOML_EDIT_OK) {
                toml_key_path_dispose(&scope);
                return TOML_EDIT_ERR;
            }
            if (header.present) {
                handled = 1;
                int event = toml_codex_event_for_path(&header.path);
                int descendant =
                    toml_codex_path_is_event_descendant(&header.path, TOML_CODEX_EVENT_SESSION) ||
                    toml_codex_path_is_event_descendant(&header.path, TOML_CODEX_EVENT_SUBAGENT);
                if (!header.array && (event != TOML_CODEX_EVENT_NONE || descendant)) {
                    toml_header_dispose(&header);
                    toml_key_path_dispose(&scope);
                    return TOML_EDIT_ERR;
                }
                scope_is_target_array =
                    header.array && (event != TOML_CODEX_EVENT_NONE || descendant);
                toml_key_path_dispose(&scope);
                scope = header.path;
                memset(&header.path, 0, sizeof(header.path));
            }
            toml_header_dispose(&header);
        }
        if (!handled && !line_in_multiline && !toml_line_is_blank_or_comment(data, &line)) {
            toml_assignment_t line_assignment;
            if (toml_parse_assignment(data, &line, &line_assignment) != TOML_EDIT_OK) {
                toml_key_path_dispose(&scope);
                return TOML_EDIT_ERR;
            }
            if (line_assignment.present) {
                toml_key_path_t full_key;
                if (toml_key_path_join(&scope, &line_assignment.key, &full_key) != TOML_EDIT_OK) {
                    toml_assignment_dispose(&line_assignment);
                    toml_key_path_dispose(&scope);
                    return TOML_EDIT_ERR;
                }
                int event = toml_codex_event_for_path(&full_key);
                if (event != TOML_CODEX_EVENT_NONE) {
                    toml_codex_assignment_view_t view;
                    if (toml_codex_assignment_at_line(data, len, &line, &view) != TOML_EDIT_OK ||
                        data[view.value_start] != '[') {
                        toml_codex_assignment_view_dispose(&view);
                        toml_key_path_dispose(&full_key);
                        toml_assignment_dispose(&line_assignment);
                        toml_key_path_dispose(&scope);
                        return TOML_EDIT_ERR;
                    }
                    toml_codex_inline_assignment_t *target =
                        event == TOML_CODEX_EVENT_SESSION ? &result->session : &result->subagent;
                    if (target->found ||
                        !(toml_codex_path_is_hooks_root(&scope) || scope.count == 0U)) {
                        toml_codex_assignment_view_dispose(&view);
                        toml_key_path_dispose(&full_key);
                        toml_assignment_dispose(&line_assignment);
                        toml_key_path_dispose(&scope);
                        return TOML_EDIT_ERR;
                    }
                    *target = (toml_codex_inline_assignment_t){
                        .found = 1,
                        .under_hooks_table = toml_codex_path_is_hooks_root(&scope),
                        .start = view.start,
                        .full_end = view.full_end,
                        .value_start = view.value_start,
                        .value_end = view.value_end,
                    };
                    cursor = view.full_end;
                    toml_codex_assignment_view_dispose(&view);
                } else if (!scope_is_target_array &&
                           toml_codex_assignment_path_conflicts(&full_key)) {
                    toml_key_path_dispose(&full_key);
                    toml_assignment_dispose(&line_assignment);
                    toml_key_path_dispose(&scope);
                    return TOML_EDIT_ERR;
                }
                toml_key_path_dispose(&full_key);
            } else if (toml_codex_path_is_hooks_root(&scope) || scope_is_target_array) {
                toml_assignment_dispose(&line_assignment);
                toml_key_path_dispose(&scope);
                return TOML_EDIT_ERR;
            }
            toml_assignment_dispose(&line_assignment);
        }
        if (!line_in_multiline && cursor > line.full_end) {
            continue;
        }
        if (toml_scan_line_strings(data, &line, &multiline_state) != TOML_EDIT_OK) {
            toml_key_path_dispose(&scope);
            return TOML_EDIT_ERR;
        }
    }
    toml_key_path_dispose(&scope);
    return multiline_state == TOML_STRING_NONE ? TOML_EDIT_OK : TOML_EDIT_ERR;
}

typedef struct {
    size_t start;
    size_t end;
    int kind;
} toml_codex_aot_hook_t;

typedef struct {
    int event;
    size_t start;
    size_t end;
    int matcher_owned;
    int unknown_descendant;
    toml_codex_aot_hook_t *hooks;
    size_t hook_count;
    size_t hook_capacity;
} toml_codex_aot_group_t;

typedef struct {
    toml_codex_aot_group_t *groups;
    size_t count;
    size_t capacity;
} toml_codex_aot_scan_t;

typedef struct {
    int active;
    int event;
    int section;
    size_t start;
    size_t hook_start;
    int unknown_descendant;
    toml_codex_field_vector_t direct_fields;
    toml_codex_field_vector_t hook_fields;
    toml_codex_aot_hook_t *hooks;
    size_t hook_count;
    size_t hook_capacity;
} toml_codex_aot_builder_t;

static void toml_codex_aot_group_dispose(toml_codex_aot_group_t *group) {
    if (!group) {
        return;
    }
    free(group->hooks);
    memset(group, 0, sizeof(*group));
}

static void toml_codex_aot_scan_dispose(toml_codex_aot_scan_t *scan) {
    if (!scan) {
        return;
    }
    for (size_t i = 0U; i < scan->count; ++i) {
        toml_codex_aot_group_dispose(&scan->groups[i]);
    }
    free(scan->groups);
    memset(scan, 0, sizeof(*scan));
}

static void toml_codex_aot_builder_dispose(toml_codex_aot_builder_t *builder) {
    if (!builder) {
        return;
    }
    toml_codex_field_vector_dispose(&builder->direct_fields);
    toml_codex_field_vector_dispose(&builder->hook_fields);
    free(builder->hooks);
    memset(builder, 0, sizeof(*builder));
}

static int toml_codex_aot_builder_push_hook(toml_codex_aot_builder_t *builder, size_t end,
                                            int kind) {
    if (!builder || !builder->active || builder->section != 1 || builder->hook_start > end ||
        builder->hook_count >= TOML_CODEX_MAX_ITEMS) {
        return TOML_EDIT_ERR;
    }
    if (builder->hook_count == builder->hook_capacity) {
        size_t capacity = builder->hook_capacity ? builder->hook_capacity * 2U : 4U;
        if (capacity > TOML_CODEX_MAX_ITEMS || capacity > SIZE_MAX / sizeof(*builder->hooks)) {
            capacity = TOML_CODEX_MAX_ITEMS;
        }
        if (capacity <= builder->hook_count) {
            return TOML_EDIT_ERR;
        }
        toml_codex_aot_hook_t *hooks =
            (toml_codex_aot_hook_t *)realloc(builder->hooks, capacity * sizeof(*hooks));
        if (!hooks) {
            return TOML_EDIT_ERR;
        }
        builder->hooks = hooks;
        builder->hook_capacity = capacity;
    }
    builder->hooks[builder->hook_count++] =
        (toml_codex_aot_hook_t){.start = builder->hook_start, .end = end, .kind = kind};
    return TOML_EDIT_OK;
}

static int toml_codex_aot_finish_hook(const char *data, toml_codex_aot_builder_t *builder,
                                      size_t end) {
    if (builder->section != 1) {
        return TOML_EDIT_OK;
    }
    int kind = toml_codex_hook_fields_kind(data, &builder->hook_fields);
    if (toml_codex_aot_builder_push_hook(builder, end, kind) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    toml_codex_field_vector_dispose(&builder->hook_fields);
    builder->section = 2;
    return TOML_EDIT_OK;
}

static int toml_codex_expected_matcher(const char *data, int event,
                                       const toml_codex_field_vector_t *fields) {
    if (fields->count != 1U) {
        return 0;
    }
    const toml_codex_field_t *matcher = toml_codex_find_field(fields, "matcher");
    const char *expected = event == TOML_CODEX_EVENT_SESSION ? "startup|resume|clear|compact" : "*";
    return matcher && toml_codex_field_string_equals(data, matcher, expected);
}

static int toml_codex_aot_scan_push_group(toml_codex_aot_scan_t *scan,
                                          toml_codex_aot_builder_t *builder, const char *data,
                                          size_t end) {
    if (!scan || !builder || !builder->active || builder->start > end ||
        scan->count >= TOML_CODEX_MAX_ITEMS ||
        toml_codex_aot_finish_hook(data, builder, end) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    if (scan->count == scan->capacity) {
        size_t capacity = scan->capacity ? scan->capacity * 2U : 8U;
        if (capacity > TOML_CODEX_MAX_ITEMS || capacity > SIZE_MAX / sizeof(*scan->groups)) {
            capacity = TOML_CODEX_MAX_ITEMS;
        }
        if (capacity <= scan->count) {
            return TOML_EDIT_ERR;
        }
        toml_codex_aot_group_t *groups =
            (toml_codex_aot_group_t *)realloc(scan->groups, capacity * sizeof(*groups));
        if (!groups) {
            return TOML_EDIT_ERR;
        }
        scan->groups = groups;
        scan->capacity = capacity;
    }
    scan->groups[scan->count++] = (toml_codex_aot_group_t){
        .event = builder->event,
        .start = builder->start,
        .end = end,
        .matcher_owned = toml_codex_expected_matcher(data, builder->event, &builder->direct_fields),
        .unknown_descendant = builder->unknown_descendant,
        .hooks = builder->hooks,
        .hook_count = builder->hook_count,
        .hook_capacity = builder->hook_capacity,
    };
    builder->hooks = NULL;
    builder->hook_count = 0U;
    builder->hook_capacity = 0U;
    toml_codex_field_vector_dispose(&builder->direct_fields);
    toml_codex_field_vector_dispose(&builder->hook_fields);
    memset(builder, 0, sizeof(*builder));
    return TOML_EDIT_OK;
}

static int toml_codex_path_is_hook_table(const toml_key_path_t *path, int event) {
    return toml_codex_path_is_event_descendant(path, event) && path->count == 3U &&
           strcmp(toml_key_path_segment(path, 2U), "hooks") == 0;
}

static int toml_codex_scan_aot_groups(const char *data, size_t len, const char *begin_marker,
                                      const char *end_marker, toml_codex_aot_scan_t *scan) {
    memset(scan, 0, sizeof(*scan));
    toml_codex_aot_builder_t builder = {0};
    size_t cursor = 0U;
    toml_line_t line;
    int multiline_state = TOML_STRING_NONE;
    while (toml_next_line(data, len, &cursor, &line)) {
        int line_in_multiline = multiline_state != TOML_STRING_NONE;
        if (!line_in_multiline && (toml_line_equals(data, &line, begin_marker) ||
                                   toml_line_equals(data, &line, end_marker))) {
            if (builder.active &&
                toml_codex_aot_scan_push_group(scan, &builder, data, line.start) != TOML_EDIT_OK) {
                goto fail;
            }
            continue;
        }
        int handled_header = 0;
        if (!line_in_multiline) {
            toml_header_t header;
            if (toml_parse_header(data, &line, "", &header) != TOML_EDIT_OK) {
                goto fail;
            }
            if (header.present) {
                handled_header = 1;
                int event = toml_codex_event_for_path(&header.path);
                if (builder.active && event != TOML_CODEX_EVENT_NONE) {
                    if (!header.array ||
                        toml_codex_aot_scan_push_group(scan, &builder, data, header.edit_start) !=
                            TOML_EDIT_OK) {
                        toml_header_dispose(&header);
                        goto fail;
                    }
                } else if (builder.active &&
                           toml_codex_path_is_event_descendant(&header.path, builder.event)) {
                    if (toml_codex_aot_finish_hook(data, &builder, header.edit_start) !=
                        TOML_EDIT_OK) {
                        toml_header_dispose(&header);
                        goto fail;
                    }
                    if (header.array &&
                        toml_codex_path_is_hook_table(&header.path, builder.event)) {
                        builder.section = 1;
                        builder.hook_start = header.edit_start;
                    } else {
                        builder.section = 2;
                        builder.unknown_descendant = 1;
                    }
                    toml_header_dispose(&header);
                    goto scan_strings;
                } else if (builder.active &&
                           toml_codex_aot_scan_push_group(scan, &builder, data,
                                                          header.edit_start) != TOML_EDIT_OK) {
                    toml_header_dispose(&header);
                    goto fail;
                }

                if (event != TOML_CODEX_EVENT_NONE) {
                    if (!header.array) {
                        toml_header_dispose(&header);
                        goto fail;
                    }
                    builder.active = 1;
                    builder.event = event;
                    builder.section = 0;
                    builder.start = header.edit_start;
                } else if (toml_codex_path_is_event_descendant(&header.path,
                                                               TOML_CODEX_EVENT_SESSION) ||
                           toml_codex_path_is_event_descendant(&header.path,
                                                               TOML_CODEX_EVENT_SUBAGENT)) {
                    toml_header_dispose(&header);
                    goto fail;
                }
            }
            toml_header_dispose(&header);
        }
        if (builder.active && !handled_header && !line_in_multiline &&
            !toml_line_is_blank_or_comment(data, &line)) {
            toml_codex_assignment_view_t assignment;
            if (toml_codex_assignment_at_line(data, len, &line, &assignment) != TOML_EDIT_OK) {
                goto fail;
            }
            int field_result = TOML_EDIT_OK;
            if (builder.section == 0) {
                field_result =
                    toml_codex_field_vector_push(&builder.direct_fields, &assignment.key,
                                                 assignment.value_start, assignment.value_end);
            } else if (builder.section == 1) {
                field_result =
                    toml_codex_field_vector_push(&builder.hook_fields, &assignment.key,
                                                 assignment.value_start, assignment.value_end);
            }
            cursor = assignment.full_end;
            toml_codex_assignment_view_dispose(&assignment);
            if (field_result != TOML_EDIT_OK) {
                goto fail;
            }
            continue;
        }
    scan_strings:
        if (toml_scan_line_strings(data, &line, &multiline_state) != TOML_EDIT_OK) {
            goto fail;
        }
    }
    if (multiline_state != TOML_STRING_NONE ||
        (builder.active &&
         toml_codex_aot_scan_push_group(scan, &builder, data, len) != TOML_EDIT_OK)) {
        goto fail;
    }
    toml_codex_aot_builder_dispose(&builder);
    return TOML_EDIT_OK;

fail:
    toml_codex_aot_builder_dispose(&builder);
    toml_codex_aot_scan_dispose(scan);
    return TOML_EDIT_ERR;
}

static int toml_codex_group_inside_marker(const toml_codex_aot_group_t *group,
                                          const toml_line_t *begin_line,
                                          const toml_line_t *end_line, int has_pair) {
    return has_pair && group->start >= begin_line->full_end && group->end <= end_line->start;
}

static int toml_codex_marker_is_owned(const char *data, size_t len,
                                      const toml_codex_aot_scan_t *scan,
                                      const toml_line_t *begin_line, const toml_line_t *end_line,
                                      int has_pair) {
    if (!has_pair) {
        return 1;
    }
    int session_current = 0;
    int subagent_current = 0;
    int session_legacy = 0;
    size_t marker_group_count = 0U;
    for (size_t i = 0U; i < scan->count; ++i) {
        const toml_codex_aot_group_t *group = &scan->groups[i];
        if (!toml_codex_group_inside_marker(group, begin_line, end_line, has_pair)) {
            if ((group->start >= begin_line->full_end && group->start < end_line->start) ||
                (group->end > begin_line->full_end && group->end <= end_line->start)) {
                return 0;
            }
            continue;
        }
        marker_group_count++;
        if (!group->matcher_owned || group->unknown_descendant || group->hook_count != 1U ||
            group->hooks[0].kind == 0) {
            return 0;
        }
        if (group->hooks[0].kind == 1) {
            if (group->event == TOML_CODEX_EVENT_SESSION) {
                session_current++;
            } else {
                subagent_current++;
            }
        } else if (group->event == TOML_CODEX_EVENT_SESSION) {
            session_legacy++;
        } else {
            return 0;
        }
    }
    size_t cursor = begin_line->full_end;
    toml_line_t line;
    while (cursor < end_line->start && toml_next_line(data, len, &cursor, &line) &&
           line.start < end_line->start) {
        if (toml_line_is_blank_or_comment(data, &line)) {
            continue;
        }
        int covered = 0;
        for (size_t i = 0U; i < scan->count; ++i) {
            const toml_codex_aot_group_t *group = &scan->groups[i];
            if (toml_codex_group_inside_marker(group, begin_line, end_line, has_pair) &&
                line.start >= group->start && line.start < group->end) {
                covered = 1;
                break;
            }
        }
        if (!covered) {
            return 0;
        }
    }
    return (marker_group_count == 2U && session_current == 1 && subagent_current == 1 &&
            session_legacy == 0) ||
           (marker_group_count == 1U && session_current == 0 && subagent_current == 0 &&
            session_legacy == 1);
}

static int toml_codex_append_quoted(toml_buffer_t *output, const char *value) {
    size_t len = 0U;
    if (!output || !value ||
        toml_bounded_length(value, TOML_EDIT_MAX_BYTES, &len) != TOML_EDIT_OK ||
        len > (TOML_EDIT_MAX_BYTES - 1U) / 6U) {
        return TOML_EDIT_ERR;
    }
    size_t escaped_size = len * 6U + 1U;
    char *escaped = (char *)malloc(escaped_size);
    if (!escaped) {
        return TOML_EDIT_ERR;
    }
    int result = cbm_toml_escape_basic_string(value, escaped, escaped_size) == TOML_EDIT_OK &&
                         toml_buffer_append_char(output, '"') == TOML_EDIT_OK &&
                         toml_buffer_append_cstr(output, escaped) == TOML_EDIT_OK &&
                         toml_buffer_append_char(output, '"') == TOML_EDIT_OK
                     ? TOML_EDIT_OK
                     : TOML_EDIT_ERR;
    free(escaped);
    return result;
}

static int toml_codex_append_raw_item(toml_buffer_t *output, const char *data,
                                      const toml_codex_span_t *item, int *has_item) {
    if (*has_item && toml_buffer_append_cstr(output, ", ") != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    if (toml_buffer_append(output, data + item->start, item->end - item->start) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    *has_item = 1;
    return TOML_EDIT_OK;
}

static int toml_codex_append_canonical_inline_event(toml_buffer_t *output, int event,
                                                    const char *command,
                                                    const char *command_windows, int *has_item) {
    const char *matcher = event == TOML_CODEX_EVENT_SESSION ? "startup|resume|clear|compact" : "*";
    if ((*has_item && toml_buffer_append_cstr(output, ", ") != TOML_EDIT_OK) ||
        toml_buffer_append_cstr(output, "{ matcher = ") != TOML_EDIT_OK ||
        toml_codex_append_quoted(output, matcher) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, ", hooks = [{ type = \"command\", command = ") !=
            TOML_EDIT_OK ||
        toml_codex_append_quoted(output, command) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, ", command_windows = ") != TOML_EDIT_OK ||
        toml_codex_append_quoted(output, command_windows) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, ", timeout = 5 }] }") != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    *has_item = 1;
    return TOML_EDIT_OK;
}

static int toml_codex_render_mixed_inline_event(toml_buffer_t *output, const char *data, int event,
                                                const toml_codex_span_vector_t *foreign_hooks,
                                                int *has_item) {
    const char *matcher = event == TOML_CODEX_EVENT_SESSION ? "startup|resume|clear|compact" : "*";
    if ((*has_item && toml_buffer_append_cstr(output, ", ") != TOML_EDIT_OK) ||
        toml_buffer_append_cstr(output, "{ matcher = ") != TOML_EDIT_OK ||
        toml_codex_append_quoted(output, matcher) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(output, ", hooks = [") != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    int has_hook = 0;
    for (size_t i = 0U; i < foreign_hooks->count; ++i) {
        if (toml_codex_append_raw_item(output, data, &foreign_hooks->items[i], &has_hook) !=
            TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
    }
    if (toml_buffer_append_cstr(output, "] }") != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    *has_item = 1;
    return TOML_EDIT_OK;
}

static int toml_codex_render_inline_array(const char *data, size_t len,
                                          const toml_codex_inline_assignment_t *assignment,
                                          int event, const char *command,
                                          const char *command_windows,
                                          cbm_toml_codex_hook_mode_t mode,
                                          toml_buffer_t *rendered) {
    toml_codex_span_vector_t events = {0};
    if (toml_codex_parse_array_items(data, len, assignment->value_start, assignment->value_end,
                                     &events) != TOML_EDIT_OK ||
        toml_buffer_append_char(rendered, '[') != TOML_EDIT_OK) {
        toml_codex_span_vector_dispose(&events);
        return TOML_EDIT_ERR;
    }
    int has_event = 0;
    int owned_found = 0;
    for (size_t i = 0U; i < events.count; ++i) {
        const toml_codex_span_t *event_span = &events.items[i];
        if (event_span->start >= event_span->end || data[event_span->start] != '{') {
            if (toml_codex_append_raw_item(rendered, data, event_span, &has_event) !=
                TOML_EDIT_OK) {
                toml_codex_span_vector_dispose(&events);
                return TOML_EDIT_ERR;
            }
            continue;
        }
        toml_codex_field_vector_t event_fields = {0};
        if (toml_codex_parse_inline_fields(data, len, event_span->start, event_span->end,
                                           &event_fields) != TOML_EDIT_OK) {
            toml_codex_field_vector_dispose(&event_fields);
            toml_codex_span_vector_dispose(&events);
            return TOML_EDIT_ERR;
        }
        const toml_codex_field_t *matcher = toml_codex_find_field(&event_fields, "matcher");
        const toml_codex_field_t *hooks = toml_codex_find_field(&event_fields, "hooks");
        const char *expected =
            event == TOML_CODEX_EVENT_SESSION ? "startup|resume|clear|compact" : "*";
        int outer_owned = event_fields.count == 2U && matcher && hooks &&
                          toml_codex_field_string_equals(data, matcher, expected) &&
                          hooks->value.start < hooks->value.end && data[hooks->value.start] == '[';
        if (!outer_owned) {
            toml_codex_field_vector_dispose(&event_fields);
            if (toml_codex_append_raw_item(rendered, data, event_span, &has_event) !=
                TOML_EDIT_OK) {
                toml_codex_span_vector_dispose(&events);
                return TOML_EDIT_ERR;
            }
            continue;
        }
        toml_codex_span_vector_t hook_items = {0};
        toml_codex_span_vector_t foreign_hooks = {0};
        if (toml_codex_parse_array_items(data, len, hooks->value.start, hooks->value.end,
                                         &hook_items) != TOML_EDIT_OK) {
            toml_codex_field_vector_dispose(&event_fields);
            toml_codex_span_vector_dispose(&hook_items);
            toml_codex_span_vector_dispose(&foreign_hooks);
            toml_codex_span_vector_dispose(&events);
            return TOML_EDIT_ERR;
        }
        int event_owned_hooks = 0;
        for (size_t hook_index = 0U; hook_index < hook_items.count; ++hook_index) {
            const toml_codex_span_t *hook_span = &hook_items.items[hook_index];
            int kind = 0;
            if (hook_span->start < hook_span->end && data[hook_span->start] == '{') {
                toml_codex_field_vector_t hook_fields = {0};
                if (toml_codex_parse_inline_fields(data, len, hook_span->start, hook_span->end,
                                                   &hook_fields) != TOML_EDIT_OK) {
                    toml_codex_field_vector_dispose(&hook_fields);
                    toml_codex_field_vector_dispose(&event_fields);
                    toml_codex_span_vector_dispose(&hook_items);
                    toml_codex_span_vector_dispose(&foreign_hooks);
                    toml_codex_span_vector_dispose(&events);
                    return TOML_EDIT_ERR;
                }
                kind = toml_codex_hook_fields_kind(data, &hook_fields);
                if (kind == 2 && event != TOML_CODEX_EVENT_SESSION) {
                    kind = 0;
                }
                toml_codex_field_vector_dispose(&hook_fields);
            }
            if (kind != 0) {
                event_owned_hooks++;
            } else if (toml_codex_span_vector_push(&foreign_hooks, hook_span->start,
                                                   hook_span->end) != TOML_EDIT_OK) {
                toml_codex_field_vector_dispose(&event_fields);
                toml_codex_span_vector_dispose(&hook_items);
                toml_codex_span_vector_dispose(&foreign_hooks);
                toml_codex_span_vector_dispose(&events);
                return TOML_EDIT_ERR;
            }
        }
        if (event_owned_hooks == 0) {
            if (toml_codex_append_raw_item(rendered, data, event_span, &has_event) !=
                TOML_EDIT_OK) {
                toml_codex_field_vector_dispose(&event_fields);
                toml_codex_span_vector_dispose(&hook_items);
                toml_codex_span_vector_dispose(&foreign_hooks);
                toml_codex_span_vector_dispose(&events);
                return TOML_EDIT_ERR;
            }
        } else {
            owned_found += event_owned_hooks;
            if (foreign_hooks.count != 0U &&
                toml_codex_render_mixed_inline_event(rendered, data, event, &foreign_hooks,
                                                     &has_event) != TOML_EDIT_OK) {
                toml_codex_field_vector_dispose(&event_fields);
                toml_codex_span_vector_dispose(&hook_items);
                toml_codex_span_vector_dispose(&foreign_hooks);
                toml_codex_span_vector_dispose(&events);
                return TOML_EDIT_ERR;
            }
        }
        toml_codex_field_vector_dispose(&event_fields);
        toml_codex_span_vector_dispose(&hook_items);
        toml_codex_span_vector_dispose(&foreign_hooks);
    }
    if (mode == CBM_TOML_CODEX_HOOK_UPSERT &&
        toml_codex_append_canonical_inline_event(rendered, event, command, command_windows,
                                                 &has_event) != TOML_EDIT_OK) {
        toml_codex_span_vector_dispose(&events);
        return TOML_EDIT_ERR;
    }
    if (toml_buffer_append_char(rendered, ']') != TOML_EDIT_OK) {
        toml_codex_span_vector_dispose(&events);
        return TOML_EDIT_ERR;
    }
    if (mode == CBM_TOML_CODEX_HOOK_REMOVE && owned_found == 0) {
        toml_buffer_dispose(rendered);
        if (toml_buffer_append(rendered, data + assignment->value_start,
                               assignment->value_end - assignment->value_start) != TOML_EDIT_OK) {
            toml_codex_span_vector_dispose(&events);
            return TOML_EDIT_ERR;
        }
    }
    toml_codex_span_vector_dispose(&events);
    return TOML_EDIT_OK;
}

typedef struct {
    size_t start;
    size_t end;
    char *replacement;
    size_t replacement_len;
} toml_codex_edit_t;

typedef struct {
    toml_codex_edit_t *items;
    size_t count;
    size_t capacity;
} toml_codex_edit_vector_t;

static void toml_codex_edit_vector_dispose(toml_codex_edit_vector_t *edits) {
    if (!edits) {
        return;
    }
    for (size_t i = 0U; i < edits->count; ++i) {
        free(edits->items[i].replacement);
    }
    free(edits->items);
    memset(edits, 0, sizeof(*edits));
}

static int toml_codex_edit_vector_push(toml_codex_edit_vector_t *edits, size_t start, size_t end,
                                       toml_buffer_t *replacement) {
    if (!edits || start > end || edits->count >= TOML_CODEX_MAX_ITEMS) {
        return TOML_EDIT_ERR;
    }
    if (edits->count == edits->capacity) {
        size_t capacity = edits->capacity ? edits->capacity * 2U : 8U;
        if (capacity > TOML_CODEX_MAX_ITEMS || capacity > SIZE_MAX / sizeof(*edits->items)) {
            capacity = TOML_CODEX_MAX_ITEMS;
        }
        if (capacity <= edits->count) {
            return TOML_EDIT_ERR;
        }
        toml_codex_edit_t *items =
            (toml_codex_edit_t *)realloc(edits->items, capacity * sizeof(*items));
        if (!items) {
            return TOML_EDIT_ERR;
        }
        edits->items = items;
        edits->capacity = capacity;
    }
    edits->items[edits->count++] = (toml_codex_edit_t){
        .start = start,
        .end = end,
        .replacement = replacement ? replacement->data : NULL,
        .replacement_len = replacement ? replacement->len : 0U,
    };
    if (replacement) {
        memset(replacement, 0, sizeof(*replacement));
    }
    return TOML_EDIT_OK;
}

static int toml_codex_edit_compare(const void *left_raw, const void *right_raw) {
    const toml_codex_edit_t *left = (const toml_codex_edit_t *)left_raw;
    const toml_codex_edit_t *right = (const toml_codex_edit_t *)right_raw;
    if (left->start < right->start) {
        return -1;
    }
    if (left->start > right->start) {
        return 1;
    }
    if (left->end < right->end) {
        return -1;
    }
    return left->end > right->end ? 1 : 0;
}

static int toml_codex_apply_edits(const char *data, size_t len, toml_codex_edit_vector_t *edits,
                                  toml_buffer_t *output) {
    if (edits->count > 1U) {
        qsort(edits->items, edits->count, sizeof(*edits->items), toml_codex_edit_compare);
    }
    size_t cursor = 0U;
    for (size_t i = 0U; i < edits->count; ++i) {
        const toml_codex_edit_t *edit = &edits->items[i];
        if (edit->start < cursor || edit->end > len ||
            toml_buffer_append(output, data + cursor, edit->start - cursor) != TOML_EDIT_OK ||
            toml_buffer_append(output, edit->replacement, edit->replacement_len) != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
        cursor = edit->end;
    }
    return toml_buffer_append(output, data + cursor, len - cursor);
}

static int toml_codex_render_new_inline_array(int event, const char *command,
                                              const char *command_windows,
                                              toml_buffer_t *rendered) {
    int has_item = 0;
    return toml_buffer_append_char(rendered, '[') == TOML_EDIT_OK &&
                   toml_codex_append_canonical_inline_event(
                       rendered, event, command, command_windows, &has_item) == TOML_EDIT_OK &&
                   toml_buffer_append_char(rendered, ']') == TOML_EDIT_OK
               ? TOML_EDIT_OK
               : TOML_EDIT_ERR;
}

static int toml_codex_add_missing_inline_edit(const char *data, const char *newline,
                                              const toml_codex_inline_assignment_t *anchor,
                                              int event, const char *command,
                                              const char *command_windows,
                                              toml_codex_edit_vector_t *edits) {
    toml_buffer_t replacement = {0};
    toml_buffer_t value = {0};
    const char *key = event == TOML_CODEX_EVENT_SESSION ? "SessionStart" : "SubagentStart";
    if (toml_codex_render_new_inline_array(event, command, command_windows, &value) !=
            TOML_EDIT_OK ||
        (anchor->full_end != 0U && data[anchor->full_end - 1U] != '\n' &&
         toml_buffer_append_cstr(&replacement, newline) != TOML_EDIT_OK) ||
        (!anchor->under_hooks_table &&
         toml_buffer_append_cstr(&replacement, "hooks.") != TOML_EDIT_OK) ||
        toml_buffer_append_cstr(&replacement, key) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(&replacement, " = ") != TOML_EDIT_OK ||
        toml_buffer_append(&replacement, value.data, value.len) != TOML_EDIT_OK ||
        toml_buffer_append_cstr(&replacement, newline) != TOML_EDIT_OK ||
        toml_codex_edit_vector_push(edits, anchor->full_end, anchor->full_end, &replacement) !=
            TOML_EDIT_OK) {
        toml_buffer_dispose(&replacement);
        toml_buffer_dispose(&value);
        return TOML_EDIT_ERR;
    }
    toml_buffer_dispose(&value);
    return TOML_EDIT_OK;
}

static int toml_codex_build_aot_block(const char *command, const char *command_windows,
                                      toml_buffer_t *block) {
    static const int events[] = {TOML_CODEX_EVENT_SESSION, TOML_CODEX_EVENT_SUBAGENT};
    for (size_t i = 0U; i < sizeof(events) / sizeof(events[0]); ++i) {
        int event = events[i];
        const char *name = event == TOML_CODEX_EVENT_SESSION ? "SessionStart" : "SubagentStart";
        const char *matcher =
            event == TOML_CODEX_EVENT_SESSION ? "startup|resume|clear|compact" : "*";
        if ((i != 0U && toml_buffer_append_char(block, '\n') != TOML_EDIT_OK) ||
            toml_buffer_append_cstr(block, "[[hooks.") != TOML_EDIT_OK ||
            toml_buffer_append_cstr(block, name) != TOML_EDIT_OK ||
            toml_buffer_append_cstr(block, "]]\nmatcher = ") != TOML_EDIT_OK ||
            toml_codex_append_quoted(block, matcher) != TOML_EDIT_OK ||
            toml_buffer_append_cstr(block, "\n\n[[hooks.") != TOML_EDIT_OK ||
            toml_buffer_append_cstr(block, name) != TOML_EDIT_OK ||
            toml_buffer_append_cstr(block, ".hooks]]\ntype = \"command\"\ncommand = ") !=
                TOML_EDIT_OK ||
            toml_codex_append_quoted(block, command) != TOML_EDIT_OK ||
            toml_buffer_append_cstr(block, "\ncommand_windows = ") != TOML_EDIT_OK ||
            toml_codex_append_quoted(block, command_windows) != TOML_EDIT_OK ||
            toml_buffer_append_cstr(block, "\ntimeout = 5\n") != TOML_EDIT_OK) {
            return TOML_EDIT_ERR;
        }
    }
    return TOML_EDIT_OK;
}

static int toml_codex_buffer_has_blank_line(const toml_buffer_t *buffer, const char *newline) {
    size_t newline_len = strlen(newline);
    return buffer->len >= newline_len * 2U &&
           memcmp(buffer->data + buffer->len - newline_len * 2U, newline, newline_len) == 0;
}

static int toml_codex_append_managed_aot(toml_buffer_t *output, const char *begin_marker,
                                         const char *end_marker, const toml_buffer_t *block,
                                         const char *newline) {
    size_t payload_start = output->len >= 3U && (unsigned char)output->data[0] == 0xefU &&
                                   (unsigned char)output->data[1] == 0xbbU &&
                                   (unsigned char)output->data[2] == 0xbfU
                               ? 3U
                               : 0U;
    if (output->len > payload_start && output->data[output->len - 1U] != '\n' &&
        toml_buffer_append_cstr(output, newline) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    if (output->len > payload_start && !toml_codex_buffer_has_blank_line(output, newline) &&
        toml_buffer_append_cstr(output, newline) != TOML_EDIT_OK) {
        return TOML_EDIT_ERR;
    }
    return toml_append_managed(output, begin_marker, end_marker, block->data, newline);
}

static int toml_codex_group_has_owned_hook(const toml_codex_aot_group_t *group) {
    if (!group->matcher_owned || group->unknown_descendant) {
        return 0;
    }
    for (size_t i = 0U; i < group->hook_count; ++i) {
        if (group->hooks[i].kind == 1 ||
            (group->hooks[i].kind == 2 && group->event == TOML_CODEX_EVENT_SESSION)) {
            return 1;
        }
    }
    return 0;
}

static int toml_codex_group_has_foreign_hook(const toml_codex_aot_group_t *group) {
    for (size_t i = 0U; i < group->hook_count; ++i) {
        if (group->hooks[i].kind == 0 ||
            (group->hooks[i].kind == 2 && group->event != TOML_CODEX_EVENT_SESSION)) {
            return 1;
        }
    }
    return 0;
}

static int toml_codex_add_aot_edits(const toml_codex_aot_scan_t *scan,
                                    const toml_line_t *begin_line, const toml_line_t *end_line,
                                    int has_pair, toml_codex_edit_vector_t *edits) {
    for (size_t i = 0U; i < scan->count; ++i) {
        const toml_codex_aot_group_t *group = &scan->groups[i];
        if (toml_codex_group_inside_marker(group, begin_line, end_line, has_pair) ||
            !toml_codex_group_has_owned_hook(group)) {
            continue;
        }
        if (!toml_codex_group_has_foreign_hook(group)) {
            if (toml_codex_edit_vector_push(edits, group->start, group->end, NULL) !=
                TOML_EDIT_OK) {
                return TOML_EDIT_ERR;
            }
            continue;
        }
        for (size_t hook_index = 0U; hook_index < group->hook_count; ++hook_index) {
            const toml_codex_aot_hook_t *hook = &group->hooks[hook_index];
            if ((hook->kind == 1 ||
                 (hook->kind == 2 && group->event == TOML_CODEX_EVENT_SESSION)) &&
                toml_codex_edit_vector_push(edits, hook->start, hook->end, NULL) != TOML_EDIT_OK) {
                return TOML_EDIT_ERR;
            }
        }
    }
    return TOML_EDIT_OK;
}

int cbm_toml_reconcile_codex_hooks(const char *file_path, const char *begin_marker,
                                   const char *end_marker, const char *command,
                                   const char *command_windows, cbm_toml_codex_hook_mode_t mode) {
    size_t command_len = 0U;
    size_t windows_len = 0U;
    if (!toml_valid_path(file_path) || !toml_valid_marker(begin_marker) ||
        !toml_valid_marker(end_marker) || strcmp(begin_marker, end_marker) == 0 || !command ||
        !command_windows ||
        (mode != CBM_TOML_CODEX_HOOK_CHECK && mode != CBM_TOML_CODEX_HOOK_UPSERT &&
         mode != CBM_TOML_CODEX_HOOK_REMOVE) ||
        toml_bounded_length(command, TOML_EDIT_MAX_BYTES, &command_len) != TOML_EDIT_OK ||
        toml_bounded_length(command_windows, TOML_EDIT_MAX_BYTES, &windows_len) != TOML_EDIT_OK ||
        !toml_codex_current_command_is_owned(command, command_len) ||
        !toml_codex_current_command_is_owned(command_windows, windows_len)) {
        return TOML_EDIT_ERR;
    }

    char *existing = NULL;
    size_t existing_len = 0U;
    toml_file_snapshot_t snapshot;
    if (toml_read_file(file_path, &existing, &existing_len, &snapshot) != TOML_EDIT_OK ||
        !toml_text_is_safe(existing, existing_len, 1) ||
        toml_validate_lexical_strings(existing, existing_len) != TOML_EDIT_OK) {
        free(existing);
        return TOML_EDIT_ERR;
    }

    toml_line_t begin_line = {0};
    toml_line_t end_line = {0};
    int has_pair = 0;
    toml_codex_inline_scan_t inline_scan;
    toml_codex_aot_scan_t aot_scan = {0};
    int result = TOML_EDIT_ERR;
    if (toml_find_markers(existing, existing_len, begin_marker, end_marker, &begin_line, &end_line,
                          &has_pair) != TOML_EDIT_OK ||
        toml_codex_scan_inline_assignments(existing, existing_len, &inline_scan) != TOML_EDIT_OK ||
        toml_codex_scan_aot_groups(existing, existing_len, begin_marker, end_marker, &aot_scan) !=
            TOML_EDIT_OK ||
        !toml_codex_marker_is_owned(existing, existing_len, &aot_scan, &begin_line, &end_line,
                                    has_pair)) {
        goto cleanup;
    }

    int inline_mode = inline_scan.session.found || inline_scan.subagent.found;
    if (inline_scan.session.found && inline_scan.subagent.found &&
        inline_scan.session.under_hooks_table != inline_scan.subagent.under_hooks_table) {
        goto cleanup;
    }
    size_t outside_aot_count = 0U;
    for (size_t i = 0U; i < aot_scan.count; ++i) {
        if (!toml_codex_group_inside_marker(&aot_scan.groups[i], &begin_line, &end_line,
                                            has_pair)) {
            outside_aot_count++;
        }
    }
    if (inline_mode && outside_aot_count != 0U) {
        goto cleanup;
    }
    cbm_toml_codex_hook_mode_t operation_mode =
        mode == CBM_TOML_CODEX_HOOK_CHECK ? CBM_TOML_CODEX_HOOK_UPSERT : mode;

    toml_codex_edit_vector_t edits = {0};
    toml_buffer_t output = {0};
    const char *newline = toml_newline_style(existing, existing_len);
    if (has_pair) {
        size_t marker_start = begin_line.start;
        if (marker_start == 0U && existing_len >= 3U && (unsigned char)existing[0] == 0xefU &&
            (unsigned char)existing[1] == 0xbbU && (unsigned char)existing[2] == 0xbfU) {
            marker_start = 3U;
        }
        if (toml_codex_edit_vector_push(&edits, marker_start, end_line.full_end, NULL) !=
            TOML_EDIT_OK) {
            goto edit_cleanup;
        }
    }
    if (inline_mode) {
        toml_codex_inline_assignment_t *assignments[] = {
            &inline_scan.session,
            &inline_scan.subagent,
        };
        const int events[] = {TOML_CODEX_EVENT_SESSION, TOML_CODEX_EVENT_SUBAGENT};
        for (size_t i = 0U; i < 2U; ++i) {
            if (!assignments[i]->found) {
                continue;
            }
            toml_buffer_t rendered = {0};
            if (toml_codex_render_inline_array(existing, existing_len, assignments[i], events[i],
                                               command, command_windows, operation_mode,
                                               &rendered) != TOML_EDIT_OK ||
                toml_codex_edit_vector_push(&edits, assignments[i]->value_start,
                                            assignments[i]->value_end, &rendered) != TOML_EDIT_OK) {
                toml_buffer_dispose(&rendered);
                goto edit_cleanup;
            }
        }
        if (operation_mode == CBM_TOML_CODEX_HOOK_UPSERT &&
            (!inline_scan.session.found || !inline_scan.subagent.found)) {
            const toml_codex_inline_assignment_t *anchor =
                inline_scan.session.found ? &inline_scan.session : &inline_scan.subagent;
            int missing =
                inline_scan.session.found ? TOML_CODEX_EVENT_SUBAGENT : TOML_CODEX_EVENT_SESSION;
            if (toml_codex_add_missing_inline_edit(existing, newline, anchor, missing, command,
                                                   command_windows, &edits) != TOML_EDIT_OK) {
                goto edit_cleanup;
            }
        }
    } else if (toml_codex_add_aot_edits(&aot_scan, &begin_line, &end_line, has_pair, &edits) !=
               TOML_EDIT_OK) {
        goto edit_cleanup;
    }

    if (toml_codex_apply_edits(existing, existing_len, &edits, &output) != TOML_EDIT_OK) {
        goto edit_cleanup;
    }
    if (!inline_mode && operation_mode == CBM_TOML_CODEX_HOOK_UPSERT) {
        toml_buffer_t block = {0};
        if (toml_codex_build_aot_block(command, command_windows, &block) != TOML_EDIT_OK ||
            toml_managed_block_conflicts(output.data, output.len, SIZE_MAX, SIZE_MAX, block.data,
                                         block.len) != TOML_EDIT_OK ||
            toml_codex_append_managed_aot(&output, begin_marker, end_marker, &block, newline) !=
                TOML_EDIT_OK) {
            toml_buffer_dispose(&block);
            goto edit_cleanup;
        }
        toml_buffer_dispose(&block);
    }
    result = mode == CBM_TOML_CODEX_HOOK_CHECK
                 ? TOML_EDIT_OK
                 : toml_write_atomic(file_path, existing, existing_len, output.data, output.len,
                                     &snapshot);

edit_cleanup:
    toml_codex_edit_vector_dispose(&edits);
    toml_buffer_dispose(&output);

cleanup:
    toml_codex_aot_scan_dispose(&aot_scan);
    free(existing);
    return result;
}
