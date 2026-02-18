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

typedef struct {
    bool initialized;
    bool time_synced;
    SemaphoreHandle_t lock;
    vprintf_like_t prev_vprintf;
    uint32_t next_id;
    size_t head;
    size_t count;
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

    for (size_t i = 0; line[i] != '\0'; ++i) {
        if (line[i] == '\r' || line[i] == '\n') {
            line[i] = '\0';
            break;
        }
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

static int log_store_vprintf(const char *fmt, va_list args)
{
    char formatted[LOG_STORE_FORMAT_BUF_MAX] = {0};
    va_list format_args;
    va_copy(format_args, args);
    (void)vsnprintf(formatted, sizeof(formatted), fmt, format_args);
    va_end(format_args);

    trim_log_message(formatted);
    if (formatted[0] == '\0') {
        return 0;
    }

    char prefix[32] = {0};
    char with_prefix[LOG_STORE_FORMAT_BUF_MAX + 48U] = {0};
    build_prefix(prefix, sizeof(prefix));
    (void)snprintf(with_prefix, sizeof(with_prefix), "[%s] %s", prefix, formatted);

    push_log_line(with_prefix);
    return log_store_prev_output("%s\n", with_prefix);
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
