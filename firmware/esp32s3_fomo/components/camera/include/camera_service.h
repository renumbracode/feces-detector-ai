#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

/* Initialise the OV3660 camera (esp32-camera driver).
 * Returns ESP_OK on success. */
esp_err_t camera_service_init(void);

/* Get the latest captured frame. Thread-safe copy from the shared buffer. */
camera_fb_t *camera_service_get_frame(void);
void camera_service_release_frame(camera_fb_t *fb);

/* The frame size the sensor actually delivered (not the requested one). The
 * actuator board's coordinate message carries it so the conversion from
 * image coordinates to angles works against the real frame. */
void camera_service_frame_size(uint16_t *w, uint16_t *h);

/* Enable / disable the streaming loop (used by the HTTP /stream handler). */
void camera_streaming_start(void);
void camera_streaming_stop(void);
