idf_component_register(SRCS "mcp.c" "mcp_server.c" "mcp_tools.c"
                            "cjson/cJSON.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "." "cjson"
                       PRIV_REQUIRES face oled esp_driver_uart esp_timer)

# cJSON 1.7.19 (MIT, see cjson/LICENSE) is bundled so the build needs no
# network: ESP-IDF 6 dropped its own json component, and the registry
# package (espressif/cjson) is the same source.
