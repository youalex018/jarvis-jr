# App sources are C only. The TFLM wrapper is components/wake_model (C++).
# Mixing .cc into src/ leaked -fuse-cxa-atexit onto C compiles.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/include
                       PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_timer esp_driver_usb_serial_jtag
                                    esp_wifi esp_netif esp_event nvs_flash lwip esp_pm
                                    tflite_microfrontend wake_model heap)
