idf_component_register(
    SRCS "main.c"
         "pet_logic.c"
         "pet_ui.c"
         "status_source.c"
         "status_bar.c"
         "devcap.c"
         "app_config.c"
         "bambu_cloud.c" "bambu_parse.c" "json_filter.c"
         "bambu_api.c"
         "net.c"
         "setup_portal.c"
         "assets/pet_frames.S"
         "assets/heart.c"
         "assets/font_pet_12.c" "assets/font_pet_8.c"
         "assets/font_pet_14.c" "assets/font_pet_15.c" "assets/font_pet_16.c" "assets/font_pet_10.c" "assets/font_pet_13.c"
         "assets/icons.c"
         "assets/font_pet_18.c"
         "assets/font_pet_24.c"
         "assets/font_pet_36.c"
    INCLUDE_DIRS "."
    EMBED_FILES "assets/portal.html.gz"
    REQUIRES bsp esp_http_client esp_http_server lwip esp_event esp_netif esp_timer esp_wifi nvs_flash mqtt json mbedtls lvgl esp_hw_support esp_driver_usb_serial_jtag
)
set_source_files_properties(assets/pet_frames.S PROPERTIES COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/assets")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/pet_frames.bin)
