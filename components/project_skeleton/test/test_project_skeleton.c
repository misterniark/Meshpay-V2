#include "meshpay/project_skeleton.h"
#include "unity.h"

TEST_CASE("project skeleton exposes stable metadata", "[project_skeleton]")
{
    TEST_ASSERT_EQUAL_STRING("MeshPayV2", meshpay_project_skeleton_name());
    TEST_ASSERT_EQUAL_UINT32(1, meshpay_project_skeleton_schema_version());
}

