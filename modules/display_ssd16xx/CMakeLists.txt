if(CONFIG_SSD16XX_EPD)
  zephyr_library()
  zephyr_library_sources(ssd16xx.c)
endif()
