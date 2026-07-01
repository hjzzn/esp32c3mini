idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES freertos esp_wifi esp_event nvs_flash esp_netif esp_timer esp_driver_gpio)