add_subdirectory(drivers/kscan)
add_subdirectory(drivers/display)

if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM AND CONFIG_SHIELD_K3YB_IT)
  zephyr_library()
  zephyr_library_sources(src/status_screen.c)
endif()
