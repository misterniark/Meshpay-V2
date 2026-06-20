#include "meshpay/hardware_smoke.h"

#include <string.h>

static const meshpay_hw_smoke_scenario_t s_scenarios[] = {
    {
        .id = "build_s3",
        .title = "Build ESP32-S3",
        .command = "./scripts/hardware_smoke.sh build-s3",
        .expected = "meshpayv2.bin is produced for esp32s3",
        .requires_flash = false,
        .requires_two_devices = false,
    },
    {
        .id = "build_cyd",
        .title = "Build CYD ESP32",
        .command = "./scripts/hardware_smoke.sh build-cyd",
        .expected = "meshpayv2.bin is produced for esp32",
        .requires_flash = false,
        .requires_two_devices = false,
    },
    {
        .id = "build_s3_secure",
        .title = "Build ESP32-S3 secure",
        .command = "./scripts/hardware_smoke.sh build-s3-secure",
        .expected = "flash encryption and encrypted NVS profile builds",
        .requires_flash = false,
        .requires_two_devices = false,
    },
    {
        .id = "flash_encrypted",
        .title = "Encrypted flash",
        .command = "MESHPAY_HW_CONFIRM=flash PORT=<port> "
                   "./scripts/hardware_smoke.sh flash-encrypted",
        .expected = "idf.py encrypted-flash completes",
        .requires_flash = true,
        .requires_two_devices = false,
    },
    {
        .id = "boot_monitor",
        .title = "Boot monitor",
        .command = "PORT=<port> ./scripts/hardware_smoke.sh monitor",
        .expected = "meshpayv2 firmware boot ready and reticulum node ready",
        .requires_flash = false,
        .requires_two_devices = false,
    },
    {
        .id = "announce_pair",
        .title = "Announce between two devices",
        .command = "Run monitor on both devices after boot",
        .expected = "each device observes at least one peer announce",
        .requires_flash = false,
        .requires_two_devices = true,
    },
    {
        .id = "payment_pair",
        .title = "Payment between two devices",
        .command = "Create a payment on device A, receive on device B",
        .expected = "sender shows confirmed and receiver shows received",
        .requires_flash = false,
        .requires_two_devices = true,
    },
    {
        .id = "sync_pair",
        .title = "DAG sync catch-up",
        .command = "Restart the lagging device and wait for DAG sync",
        .expected = "lagging device applies the missing batch",
        .requires_flash = false,
        .requires_two_devices = true,
    },
};

const meshpay_hw_smoke_scenario_t *meshpay_hw_smoke_scenarios(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(s_scenarios) / sizeof(s_scenarios[0]);
    }
    return s_scenarios;
}

const meshpay_hw_smoke_scenario_t *meshpay_hw_smoke_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    size_t count = 0;
    const meshpay_hw_smoke_scenario_t *scenarios =
        meshpay_hw_smoke_scenarios(&count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(scenarios[i].id, id) == 0) {
            return &scenarios[i];
        }
    }
    return NULL;
}

esp_err_t meshpay_hw_smoke_validate_manifest(void)
{
    size_t count = 0;
    const meshpay_hw_smoke_scenario_t *scenarios =
        meshpay_hw_smoke_scenarios(&count);
    if (scenarios == NULL || count < 7) {
        return ESP_ERR_INVALID_STATE;
    }

    bool has_flash_guard = false;
    bool has_two_device_flow = false;
    for (size_t i = 0; i < count; ++i) {
        const meshpay_hw_smoke_scenario_t *scenario = &scenarios[i];
        if (scenario->id == NULL || scenario->id[0] == '\0' ||
            scenario->title == NULL || scenario->title[0] == '\0' ||
            scenario->command == NULL || scenario->command[0] == '\0' ||
            scenario->expected == NULL || scenario->expected[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
        for (size_t j = i + 1; j < count; ++j) {
            if (strcmp(scenario->id, scenarios[j].id) == 0) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        if (scenario->requires_flash &&
            strstr(scenario->command, "MESHPAY_HW_CONFIRM=flash") != NULL) {
            has_flash_guard = true;
        }
        if (scenario->requires_two_devices) {
            has_two_device_flow = true;
        }
    }

    return (has_flash_guard && has_two_device_flow) ? ESP_OK
                                                    : ESP_ERR_INVALID_STATE;
}
