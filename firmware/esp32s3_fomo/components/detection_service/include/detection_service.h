#pragma once

#include "esp_err.h"

/* Continuous detection loop: captures frames on a runner, runs FOMO
 * inference, triggers the relay on detection, and reports events to the
 * CodeIgniter dashboard + Python YOLOv8 verifier. Runs on a dedicated core. */
esp_err_t detection_service_start(void);
