# Requirements are expanded before project options/config are available. Include
# esp_eth for the optional S3 capture profile; hardware S3 still starts only WiFi.
set(network_deps esp_netif esp_event nvs_flash mdns esp_eth)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND network_deps esp_wifi)
else()
    list(APPEND network_deps esp_driver_gpio)
endif()
idf_component_register(SRCS "main.cpp" "platform_esp32.cpp" "network.cpp" "serial_console.cpp" "improv.cpp"
    "sd_storage.cpp"
    INCLUDE_DIRS "../include"
    PRIV_REQUIRES mcore esp_psram esp_timer esp_system esp_hw_support esp_app_format vfs lwip esp_driver_uart
    esp_driver_usb_serial_jtag fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi esp_driver_spi ${network_deps})
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>")
target_compile_definitions(${COMPONENT_LIB} PRIVATE MC_MIN_PSRAM_MB=${MC_PSRAM_MB})
if(MC_EMULATOR)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MC_EMULATOR MC_NBD_PORT=${MC_NBD_PORT})
endif()
if(MC_QEMU_CAPTURE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MC_QEMU_CAPTURE MC_NBD_PORT=${MC_NBD_PORT})
endif()
if(MC_RELEASE_CONFIG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MC_RELEASE_CONFIG)
endif()
if(MC_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MC_BENCH)
endif()
if(MC_CPU_PROFILE)
    target_sources(${COMPONENT_LIB} PRIVATE "cpu_profile.cpp")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MC_CPU_PROFILE)
endif()
