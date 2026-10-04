#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Connect to the configured Wi-Fi AP (or start a soft-AP if the network
 * is unreachable). Blocks until an IP is obtained (or AP started). */
esp_err_t wifi_service_start(void);

/* Signal strength of the associated AP, in dBm (roughly -30..-90).
 * Returns INT8_MIN when the STA is not associated. RSSI is the single most
 * useful number when diagnosing a marginal link: a board stuck near -80 while
 * a phone at the same spot reports "excellent" means the antenna is detuned,
 * which no firmware change can fix. */
int8_t wifi_service_rssi(void);

/* True once an IP has been obtained, false while disconnected. */
bool wifi_service_is_connected(void);

/* Number of consecutive reconnect attempts since the last successful
 * association. Exposed so /status can show whether the link is degrading. */
uint32_t wifi_service_reconnect_attempts(void);
