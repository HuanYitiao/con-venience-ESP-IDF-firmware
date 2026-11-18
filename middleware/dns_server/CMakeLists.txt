# Copied unchanged from ESP-IDF v6.1 examples/protocols/http_server/captive_portal
# (Unlicense OR CC0-1.0). Answers every A query with the soft-AP address.
idf_component_register(SRCS dns_server.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES esp_netif)
