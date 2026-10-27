#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* The HTTP server on the main board.
 *
 * Endpoints:
 *   POST /target            a detection from the camera board (the protocol
 *                           is docs/target-protocol.md)
 *   GET  /status            everything the dashboard needs in one document
 *   GET  /servo?pan=&tilt=  manual servo test -- NO pump
 *   GET  /pump?duration=    manual pump test -- water guarded
 *   GET  /config            read the calibration back
 *   POST /config            change the calibration (and reboot)
 *   GET  /setup             a small form for the calibration
 */

typedef struct {
    const char *wifi_ssid;
    const char *wifi_pass;
    uint16_t    http_port;
} target_server_opts_t;

esp_err_t target_server_start(const target_server_opts_t *opts);
