set(SRCS
    "main.cpp"
    "app_manager.cpp"
    "menu.cpp"
    "lcd1602.cpp"
    "sd_manager.cpp"
    "wifi_manager.cpp"
    "captive_portal.cpp"
    "captive_portal_html.c"
    "apps/app_sysmon.cpp"
    "apps/app_settings.cpp"
    "apps/app_camera.cpp"
    "apps/app_files.cpp"
    "backlight.cpp"
    "idle.cpp"
    "timezone.cpp"
    "screenshot.cpp"
    "file_server.cpp"
    "thumbnail.cpp"
    "lv_mem_custom.cpp"
    "spotify_auth.cpp"
    "spotify_client.cpp"
    "apps/app_spotify.cpp"
    "fonts/font_gohu11.c"
    "fonts/font_gohu14.c"
    "fonts/font_gohu14b.c"
)

idf_component_register(SRCS ${SRCS}
                        INCLUDE_DIRS "." "apps" "fonts")

# Force linker to include custom LVGL malloc symbols (LVGL links before main)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u lv_mem_init"
    "-u lv_mem_deinit"
    "-u lv_malloc_core"
    "-u lv_realloc_core"
    "-u lv_free_core"
)
