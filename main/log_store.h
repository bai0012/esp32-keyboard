#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t id;
    char line[192];
} log_store_entry_t;

esp_err_t log_store_init(void);
void log_store_mark_time_synced(void);
bool log_store_is_time_synced(void);
size_t log_store_copy_recent(log_store_entry_t *out_entries, size_t out_cap, size_t limit);
