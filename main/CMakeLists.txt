idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES project_skeleton rns_node rns_radio rns_announce app_main device_hal dag_sync dag_monitor ui esp_timer
)
