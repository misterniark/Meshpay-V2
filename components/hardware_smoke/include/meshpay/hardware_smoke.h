#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESHPAY_HW_SMOKE_MANIFEST_VERSION 1

typedef struct {
    const char *id;
    const char *title;
    const char *command;
    const char *expected;
    bool requires_flash;
    bool requires_two_devices;
} meshpay_hw_smoke_scenario_t;

const meshpay_hw_smoke_scenario_t *meshpay_hw_smoke_scenarios(size_t *count);
const meshpay_hw_smoke_scenario_t *meshpay_hw_smoke_find(const char *id);
esp_err_t meshpay_hw_smoke_validate_manifest(void);

#ifdef __cplusplus
}
#endif
