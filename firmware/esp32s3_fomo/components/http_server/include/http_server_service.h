#pragma once

#include "esp_err.h"

/* Start the HTTP server (port 80): /stream, /status, /spray, /config. */
esp_err_t http_server_service_init(void);
