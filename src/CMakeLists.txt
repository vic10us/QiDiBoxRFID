idf_component_register(
    SRCS "main.c" "pn532.c" "wifi.c" "webserver.c" "led.c" "config.c" "dns_server.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_http_server driver esp_event led_strip
)
