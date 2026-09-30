# Hardware Wiring Guide

ESP32-S3 + OV3660 camera + alarm output. All pin assignments below are read from
the firmware, not from documentation, so they match what the code actually does.

## Pin assignments

Defined in `firmware/esp32s3_fomo/components/camera/camera_service.c` and
`firmware/esp32s3_fomo/components/app_config/include/app_config.h`.

| ESP32-S3 | Camera (OV3660) | Function |
|----------|-----------------|----------|
| 15 | XCLK  | 20 MHz clock out from ESP32 |
| 4  | SIOD  | SCCB data (I2C-like) |
| 5  | SIOC  | SCCB clock |
| 6  | VSYNC | vertical sync |
| 7  | HREF  | horizontal ref |
| 11 | D0    | parallel data bit 0 |
| 8  | D2    | parallel data bit 2 |
| 9  | D1    | parallel data bit 1 |
| 10 | D3    | parallel data bit 3 |
| 12 | D4    | parallel data bit 4 |
| 13 | PCLK  | pixel clock |
| 16 | D7    | parallel data bit 7 |
| 17 | D6    | parallel data bit 6 |
| 18 | D5    | parallel data bit 5 |
| 3V3 | VCC   | power |
| GND | GND   | ground |

PWDN and RESET are disabled in firmware (`-1`), so those two camera pins should be
tied low or left unconnected. Confirm against your module's datasheet.

## Alarm output

| Part | ESP32-S3 | Wiring |
|------|----------|--------|
| Buzzer (3-5V) | GPIO14 | GPIO14 -> 220 ohm resistor -> buzzer + ; buzzer - -> GND |
| LED | GPIO1 | GPIO1 -> 330 ohm resistor -> LED anode ; LED cathode -> GND |
| On-board status LED | GPIO2 | no wiring, already on the devkit |

The resistors are required. An LED driven straight from a GPIO pin will overcurrent
the pin and can brownout the board.

The buzzer is driven at roughly 2 kHz using LEDC TIMER_1/CHANNEL_1, which sounds both
**active** buzzers (internal oscillator, ignores the modulation) and **passive**
buzzers (needs the frequency). A passive buzzer on a plain GPIO toggle would only
click silently, so the tone generation is deliberate. The camera uses LEDC
TIMER_0/CHANNEL_0, so the two do not collide.

## Do not use GPIO 4 for the relay

`APP_PIN_RELAY` was originally 4, which is the camera's SCCB data line. The camera
initializes *after* the spray controller, so it reclaims GPIO4 as an open-drain
input. Driving it then does nothing at all, while the log still prints `Spray ON` --
a failure that looks like a success in the logs.

`APP_PIN_RELAY` is now 14. If you wire a relay, wire it there.

## Adding a relay and water pump (later)

Not required for the LED + buzzer mock defense.

- Relay `IN` -> GPIO14 through 1k ohm, alongside the buzzer on the same pin
- Relay `VCC` -> 5V if the coil is 5V, otherwise 3V3
- Relay `GND` -> GND
- Pump positive -> relay `NO` (normally open) terminal
- Pump negative -> GND
- Relay `COM` -> pump supply positive

Use `NO`, not `NC`. With `NO`, a crash or brownout leaves the pump off. With `NC`,
a failure leaves the pump running with no way to stop it.

Check whether your relay module is active-low before wiring. Touch a multimeter
continuity probe between `GND` and `IN`: if the relay clicks, it is active-low and
`IN` must be pulled low to fire. Most single-channel modules (`KY-019`,
`SRD-05VDC-SL-C`) are active-low, which is fail-safe because a floating or crashed
pin means "off".

Add a flyback diode (1N4007, cathode toward the positive terminal) across the pump.
Never drive pump current through a GPIO pin -- the relay contacts switch the power
and the ESP32 only ever drives the coil.

If the pump draws more than about 1A, or runs on 12V, give it a separate supply and
share only the ground. Never feed 12V into a GPIO.

## First power-up checklist

1. With power disconnected, confirm no stray wire is on GPIO4.
2. Confirm the buzzer and LED resistors are installed, not the parts direct.
3. Power on and check the board boots and joins Wi-Fi.
4. `curl http://192.168.1.39/status` and confirm `sprayActive` reads `false`.
5. Press **Spray now** in the dashboard. The LED, the on-board LED, and the buzzer
   should all activate for 5 seconds, and the serial log should show
   `Spray ON (5000 ms)` then `Spray OFF`.
6. Note that this starts the 5 minute cooldown, so do it before a real detection
   test rather than between attempts.

## Pins to avoid

| Pins | Reason |
|------|--------|
| 0, 3, 45, 46 | strapping pins, affect boot |
| 19, 20 | USB D-/D+ |
| 26-32 | SPI flash |
| 33-37 | octal PSRAM |

Free and safe: 1, 14, 21.
