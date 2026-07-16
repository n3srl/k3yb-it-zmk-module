if(CONFIG_K3YB_KSCAN_GPIO_DEMUX)
  zephyr_library()
  zephyr_library_sources(kscan_gpio_demux_settle.c)
endif()
