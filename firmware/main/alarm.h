#pragma once
#include <stddef.h>

/* Loads the stored config (or defaults) and starts the sampling task. */
void alarm_start(void);

/* Hooks for fc_agent_config_t. */
int alarm_on_config_file(const char *path, const char *data, size_t len, void *ctx);
void alarm_custom_info(void *cjson_obj, void *ctx);
