#include "log_store.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log_timestamp.h"
#include "esp_log_write.h"

#define LOG_STORE_MAX_ENTRIES 240U
#define LOG_STORE_LINE_MAX 192U
#define LOG_STORE_FORMAT_BUF_MAX 320U
#define LOG_STORE_ACCUM_MAX 768U
#define LOG_STORE_MAX_EMIT_LINES 12U

typedef struct {
    bool initialized;
    bool time_synced;
    SemaphoreHandle_t lock;
    vprintf_like_t prev_vprintf;
    uint32_t next_id;
    size_t head;
    size_t count;
    size_t accum_len;
    bool pending_tag_line;
    char accum[LOG_STORE_ACCUM_MAX];
    char pending_monitor_prefix[32];
    char pending_tag_payload[48];
    log_store_entry_t entries[LOG_STORE_MAX_ENTRIES];
} log_store_state_t;

static log_store_state_t s_log_store = {0};

static bool is_wall_time_valid(void)
{
    time_t now = 0;
    time(&now);
    if (now <= 0) {
        return false;
    }

    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2020 - 1900);
}

static void trim_log_message(char *line)
{
    if (line == NULL) {
        return;
    }
    size_t len = strlen(line);
    while (len > 0U && (line[len - 1U] == '\r' || line[len - 1U] == '\n')) {
        line[len - 1U] = '\0';
        len--;
    }
}

static int log_store_prev_output(const char *fmt, ...)
{
    if (s_log_store.prev_vprintf == NULL || fmt == NULL) {
        return 0;
    }

    va_list args;
    va_start(args, fmt);
    const int ret = s_log_store.prev_vprintf(fmt, args);
    va_end(args);
    return ret;
}

static void append_fragment_collect_lines(const char *fragment,
                                          char out_lines[LOG_STORE_MAX_EMIT_LINES][LOG_STORE_FORMAT_BUF_MAX],
                                          size_t *out_count)
{
    if (fragment == NULL || out_lines == NULL || out_count == NULL) {
        return;
    }

    for (size_t i = 0; fragment[i] != '\0'; ++i) {
        const char c = fragment[i];
        if (s_log_store.accum_len < (LOG_STORE_ACCUM_MAX - 1U)) {
            s_log_store.accum[s_log_store.accum_len++] = c;
            s_log_store.accum[s_log_store.accum_len] = '\0';
        } else {
            if (*out_count < LOG_STORE_MAX_EMIT_LINES) {
                strlcpy(out_lines[*out_count], s_log_store.accum, LOG_STORE_FORMAT_BUF_MAX);
                (*out_count)++;
            }
            s_log_store.accum_len = 0U;
            s_log_store.accum[0] = '\0';
            if (c != '\n') {
                s_log_store.accum[s_log_store.accum_len++] = c;
                s_log_store.accum[s_log_store.accum_len] = '\0';
            }
        }

        if (c == '\n') {
            if (*out_count < LOG_STORE_MAX_EMIT_LINES) {
                strlcpy(out_lines[*out_count], s_log_store.accum, LOG_STORE_FORMAT_BUF_MAX);
                (*out_count)++;
            }
            s_log_store.accum_len = 0U;
            s_log_store.accum[0] = '\0';
        }
    }
}

static void build_prefix(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }

    if (s_log_store.time_synced || is_wall_time_valid()) {
        s_log_store.time_synced = true;
        time_t now = 0;
        time(&now);
        struct tm timeinfo = {0};
        localtime_r(&now, &timeinfo);
        (void)strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &timeinfo);
    } else {
        (void)snprintf(out, out_size, "+%lu ms", (unsigned long)esp_log_timestamp());
    }
}

static void push_log_line(const char *line)
{
    if (!s_log_store.initialized || line == NULL || line[0] == '\0') {
        return;
    }

    if (s_log_store.lock != NULL) {
        (void)xSemaphoreTake(s_log_store.lock, portMAX_DELAY);
    }
    log_store_entry_t *dst = &s_log_store.entries[s_log_store.head];
    dst->id = ++s_log_store.next_id;
    strlcpy(dst->line, line, sizeof(dst->line));

    s_log_store.head = (s_log_store.head + 1U) % LOG_STORE_MAX_ENTRIES;
    if (s_log_store.count < LOG_STORE_MAX_ENTRIES) {
        s_log_store.count++;
    }
    if (s_log_store.lock != NULL) {
        (void)xSemaphoreGive(s_log_store.lock);
    }
}

static int emit_output_line(const char *monitor_prefix, bool has_monitor_prefix, const char *payload)
{
    char time_prefix[32] = {0};
    char with_prefix[LOG_STORE_FORMAT_BUF_MAX + 80U] = {0};
    build_prefix(time_prefix, sizeof(time_prefix));
    if (has_monitor_prefix) {
        (void)snprintf(with_prefix, sizeof(with_prefix), "%s [%s] %s", monitor_prefix, time_prefix, payload);
    } else {
        (void)snprintf(with_prefix, sizeof(with_prefix), "[%s] %s", time_prefix, payload);
    }

    push_log_line(with_prefix);
    return log_store_prev_output("%s\n", with_prefix);
}

static int emit_rewritten_line(const char *raw_line)
{
    if (raw_line == NULL) {
        return 0;
    }

    char formatted[LOG_STORE_FORMAT_BUF_MAX] = {0};
    strlcpy(formatted, raw_line, sizeof(formatted));
    trim_log_message(formatted);
    if (formatted[0] == '\0') {
        return 0;
    }

    bool has_monitor_prefix = false;
    char monitor_prefix[32] = {0};
    const char *payload = formatted;

    if (formatted[0] != '\0' && formatted[1] == ' ' && formatted[2] == '(') {
        const char *end_ts = strstr(formatted, ") ");
        if (end_ts != NULL) {
            const size_t prefix_len = (size_t)(end_ts - formatted + 2U);
            if (prefix_len < sizeof(monitor_prefix)) {
                memcpy(monitor_prefix, formatted, prefix_len);
                monitor_prefix[prefix_len] = '\0';
                has_monitor_prefix = true;
                if (end_ts[2] != '\0') {
                    payload = end_ts + 2;
                } else {
                    payload = "";
                }
            }
        }
    }

    int total = 0;
    if (has_monitor_prefix && s_log_store.pending_tag_line) {
        total += emit_output_line(s_log_store.pending_monitor_prefix, true, s_log_store.pending_tag_payload);
        s_log_store.pending_tag_line = false;
        s_log_store.pending_monitor_prefix[0] = '\0';
        s_log_store.pending_tag_payload[0] = '\0';
    }

    if (!has_monitor_prefix && s_log_store.pending_tag_line) {
        char merged_payload[LOG_STORE_FORMAT_BUF_MAX] = {0};
        (void)snprintf(merged_payload,
                       sizeof(merged_payload),
                       "%s %s",
                       s_log_store.pending_tag_payload,
                       payload);
        total += emit_output_line(s_log_store.pending_monitor_prefix, true, merged_payload);
        s_log_store.pending_tag_line = false;
        s_log_store.pending_monitor_prefix[0] = '\0';
        s_log_store.pending_tag_payload[0] = '\0';
        return total;
    }

    if (has_monitor_prefix) {
        const size_t payload_len = strlen(payload);
        const bool tag_only_line =
            payload_len > 0U &&
            payload[payload_len - 1U] == ':' &&
            strchr(payload, ' ') == NULL;
        if (tag_only_line) {
            s_log_store.pending_tag_line = true;
            strlcpy(s_log_store.pending_monitor_prefix,
                    monitor_prefix,
                    sizeof(s_log_store.pending_monitor_prefix));
            strlcpy(s_log_store.pending_tag_payload, payload, sizeof(s_log_store.pending_tag_payload));
            return total;
        }
        total += emit_output_line(monitor_prefix, true, payload);
    } else {
        total += emit_output_line(NULL, false, payload);
    }
    return total;
}

static int log_store_vprintf(const char *fmt, va_list args)
{
    char formatted[LOG_STORE_FORMAT_BUF_MAX] = {0};
    va_list format_args;
    va_copy(format_args, args);
    (void)vsnprintf(formatted, sizeof(formatted), fmt, format_args);
    va_end(format_args);

    if (formatted[0] == '\0') {
        return 0;
    }

    char lines[LOG_STORE_MAX_EMIT_LINES][LOG_STORE_FORMAT_BUF_MAX] = {0};
    size_t line_count = 0U;

    if (s_log_store.lock != NULL) {
        (void)xSemaphoreTake(s_log_store.lock, portMAX_DELAY);
    }
    append_fragment_collect_lines(formatted, lines, &line_count);
    if (s_log_store.lock != NULL) {
        (void)xSemaphoreGive(s_log_store.lock);
    }

    int total = 0;
    for (size_t i = 0; i < line_count; ++i) {
        total += emit_rewritten_line(lines[i]);
    }

    return total;
}

esp_err_t log_store_init(void)
{
    if (s_log_store.initialized) {
        return ESP_OK;
    }

    memset(&s_log_store, 0, sizeof(s_log_store));
    s_log_store.lock = xSemaphoreCreateMutex();
    if (s_log_store.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_log_store.time_synced = is_wall_time_valid();
    s_log_store.prev_vprintf = esp_log_set_vprintf(log_store_vprintf);
    s_log_store.initialized = true;
    return ESP_OK;
}

void log_store_mark_time_synced(void)
{
    s_log_store.time_synced = true;
}

bool log_store_is_time_synced(void)
{
    return s_log_store.time_synced || is_wall_time_valid();
}

size_t log_store_copy_recent(log_store_entry_t *out_entries, size_t out_cap, size_t limit)
{
    if (!s_log_store.initialized || out_entries == NULL || out_cap == 0U) {
        return 0U;
    }

    if (s_log_store.lock != NULL) {
        (void)xSemaphoreTake(s_log_store.lock, portMAX_DELAY);
    }

    const size_t available = s_log_store.count;
    size_t wanted = limit;
    if (wanted == 0U || wanted > available) {
        wanted = available;
    }
    if (wanted > out_cap) {
        wanted = out_cap;
    }

    size_t start = 0U;
    if (available > wanted) {
        start = (s_log_store.head + LOG_STORE_MAX_ENTRIES - wanted) % LOG_STORE_MAX_ENTRIES;
    } else {
        start = (s_log_store.head + LOG_STORE_MAX_ENTRIES - available) % LOG_STORE_MAX_ENTRIES;
    }

    for (size_t i = 0; i < wanted; ++i) {
        const size_t src_idx = (start + i) % LOG_STORE_MAX_ENTRIES;
        out_entries[i] = s_log_store.entries[src_idx];
    }

    if (s_log_store.lock != NULL) {
        (void)xSemaphoreGive(s_log_store.lock);
    }
    return wanted;
}
