#pragma once

#include "esp_err.h"

/* Connect to the configured Wi-Fi AP (or start a soft-AP if the network
 * is unreachable). Blocks until an IP is obtained (or AP started). */
esp_err_t wifi_service_start(void);
