idf_component_register(
    SRCS "hub_proto.c" "hub_turns.c" "hub_http.c" "hub_side_chat.c" "hub_main.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES esp_http_server esp_netif esp_timer esp_hw_support log muse nvs_flash
    WHOLE_ARCHIVE
)
