#include "meshpay/hardware_smoke.h"
#include "unity.h"

#include <string.h>

TEST_CASE("hardware smoke manifest is complete and valid", "[hardware_smoke]")
{
    TEST_ASSERT_EQUAL(ESP_OK, meshpay_hw_smoke_validate_manifest());

    size_t count = 0;
    const meshpay_hw_smoke_scenario_t *scenarios =
        meshpay_hw_smoke_scenarios(&count);
    TEST_ASSERT_NOT_NULL(scenarios);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(7, count);

    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("build_s3"));
    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("build_cyd"));
    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("build_s3_secure"));
    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("flash_encrypted"));
    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("boot_monitor"));
    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("announce_pair"));
    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("payment_pair"));
    TEST_ASSERT_NOT_NULL(meshpay_hw_smoke_find("sync_pair"));
    TEST_ASSERT_NULL(meshpay_hw_smoke_find("missing"));
}

TEST_CASE("hardware smoke flags destructive and pair scenarios",
          "[hardware_smoke]")
{
    const meshpay_hw_smoke_scenario_t *flash =
        meshpay_hw_smoke_find("flash_encrypted");
    TEST_ASSERT_NOT_NULL(flash);
    TEST_ASSERT_TRUE(flash->requires_flash);
    TEST_ASSERT_NOT_NULL(strstr(flash->command, "MESHPAY_HW_CONFIRM=flash"));

    const meshpay_hw_smoke_scenario_t *payment =
        meshpay_hw_smoke_find("payment_pair");
    TEST_ASSERT_NOT_NULL(payment);
    TEST_ASSERT_TRUE(payment->requires_two_devices);
    TEST_ASSERT_FALSE(payment->requires_flash);
}
