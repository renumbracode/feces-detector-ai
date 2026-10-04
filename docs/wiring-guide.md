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

Each load gets its **own** pin. Sharing one pin between two outputs does not work —
see the note below.

| Part | ESP32-S3 | Wiring |
|------|----------|--------|
| Relay IN | GPIO14 | GPIO14 -> 1k ohm -> relay `IN` ; relay `GND` -> GND |
| Buzzer (3-5V) | GPIO21 | GPIO21 -> 220 ohm resistor -> buzzer + ; buzzer - -> GND |
| LED | GPIO1 | GPIO1 -> 330 ohm resistor -> LED anode ; LED cathode -> GND |

The resistors are required. An LED driven straight from a GPIO pin will overcurrent
the pin and can brownout the board.

GPIO2 is unused. On several ESP32-S3 devkits it is an addressable RGB LED that needs
a NeoPixel/RMT bitstream and does nothing useful as a plain GPIO.

### The buzzer must not share the relay pin

The buzzer here is an **active** type (it has its own oscillator) and is driven by a
**steady DC level** on GPIO21, exactly like the LED and the relay.

An earlier revision drove it with 2 kHz PWM from LEDC, on the assumption that one drive
would cover both active and passive parts. That is wrong for an active buzzer: it wants
continuous DC, and a duty-cycled 3.3 V pin averages about 1.65 V with constant edge
transitions — too quiet to hear through the series resistor. It sat silent while the
dashboard and the serial log cheerfully reported `buzzer=21`, because both record the
command, never the electrical result.

**This build suits an active buzzer only.** A *passive* buzzer needs a tone and would
only click on a plain GPIO level; it would need LEDC brought back. Check the part
before swapping it.

Each load still needs its own pin. Two GPIO outputs on one pad drive the line against
each other, so a shared pin gives a relay that never latches cleanly — and again the
log reports `Spray ON` regardless.

### Relay IN polarity

Most single-channel modules (`KY-019`, `SRD-05VDC-SL-C`) are **active-low**: the coil
energises when `IN` is pulled to GND. That is fail-safe, because a floating or
crashed pin then means "pump off". Active-high boards also exist, so the firmware
treats polarity as a setting rather than an assumption.

Confirm yours before trusting the default. Touch a multimeter continuity probe
between `GND` and `IN`: if the relay clicks, it is active-low and `IN` must be pulled
low to fire.

The setting is stored in NVS (`relay_active_low`) and defaults to active-low:

```bash
# read it back
curl http://192.168.1.39/config

# switch to active-HIGH (reboots the board)
curl -X POST http://192.168.1.39/config -d 'relay_active_low=0'
```

It is also selectable on the `/setup` page. If the LED and buzzer respond to
`GET /spray?duration=3000` but the relay stays silent, the module is active-high —
flip this and reboot. No reflash needed.

## Do not use GPIO 4 for the relay

`APP_PIN_RELAY` was originally 4, which is the camera's SCCB data line. The camera
initializes *after* the spray controller, so it reclaims GPIO4 as an open-drain
input. Driving it then does nothing at all, while the log still prints `Spray ON` --
a failure that looks like a success in the logs.

`APP_PIN_RELAY` is now 14. If you wire a relay, wire it there.

The same failure mode has a second, sneakier form: **two loads on one pin.** An
earlier revision put the buzzer and the relay both on GPIO14, where two outputs
drive against each other. It looks the same from the outside. If the
serial log says `Spray ON` and nothing moves, suspect a pin collision before
suspecting the model.

## Adding a relay and water pump (later)

Not required for the LED + buzzer mock defense.

- Relay `IN` -> GPIO14 through 1k ohm
- Relay `VCC` -> 5V if the coil is 5V, otherwise 3V3
- Relay `GND` -> GND
- Pump positive -> relay `NO` (normally open) terminal
- Pump negative -> GND
- Relay `COM` -> pump supply positive

Use `NO`, not `NC`. With `NO`, a crash or brownout leaves the pump off. With `NC`,
a failure leaves the pump running with no way to stop it.

Set `relay_active_low` to match your module -- see "Relay IN polarity" above. Most
single-channel modules are active-low, which is fail-safe because a floating or
crashed pin means "off".

Add a flyback diode (1N4007, cathode toward the positive terminal) across the pump.
Never drive pump current through a GPIO pin -- the relay contacts switch the power
and the ESP32 only ever drives the coil.

### Pump power: use a separate supply

Feed the pump from its **own supply**, and share **only the ground** with the ESP32.
Never take pump current from the board's 5 V pin or a GPIO.

This is not theoretical. A brownout during testing — the board resetting as the pump
started — is what happens when the board's rail is asked to carry pump inrush. The
ESP32-S3 draws 300-380 mA on its own (Wi-Fi TX + 8 MB PSRAM + camera + inference) and
has little bulk capacitance, so it collapses first. The brownout detector reset the
chip; unplugging "fixed" it only because that removed the load.

A 5 V relay **coil** on the board's 5 V pin is fine — it is a short, low-current
draw. It is the pump's `COM` supply that must be separate.

If the pump draws more than about 1A, or runs on 12V, it definitely needs its own
supply. Never feed 12V into a GPIO.

## Network notes that matter on a weak link

These came out of testing and are worth reading before a demo.

**Set the AP's 2.4 GHz channel width to 20 MHz.** The ESP32-S3 is 20 MHz-only. On a
40 MHz channel it receives the beacon and traffic across 40 MHz but can only decode
20 MHz of it, so the extra half is pure interference from the same access point. The
symptom is strong RSSI (better than -50 dBm) with poor packet delivery, which is
confusing because the signal looks perfect. Measured on a phone hotspot: 3-53% packet
loss with RSSI swinging -33 to -62 dBm.

**Prefer a static address.** Every dashboard `/history` and `/live` URL and the manual
"Spray now" button hardcode the device address, so when DHCP hands out a new one the
dashboard silently points at nothing and looks like a dead detector. The firmware
supports an optional static IP (`static_ip` / `static_mask` / `static_gw` over
`POST /config`, all three together or all blank) and this board is pinned to
`192.168.1.39`. A router-side DHCP reservation does the same with no firmware coupling.

**Watch `/status` for link health.** `wifiRssi`, `wifiReconnects`, and on the verifier
leg `verifyOk` / `verifyFailures` / `verifyOkAgeMs`. `ageMs` climbing on its own with no
reconnects means the board has stopped attempting the poll, not that the server is
down — that distinction is not visible anywhere else.

**Server-gated spray depends on this path.** The YOLOv8 trigger needs the PC reachable;
on a lossy link the poll leg fails and that trigger goes quiet, while the on-device
FOMO path keeps working. `verifyFailures` counts the attempts so you can tell which
side is at fault.

## First power-up checklist

1. With power disconnected, confirm no stray wire is on GPIO4, and that the
   buzzer is on **GPIO21** — not on GPIO14 with the relay.
2. Confirm the buzzer and LED resistors are installed, not the parts direct.
3. **With the pump powered but the board idle, confirm the pump is OFF.** A pump that
   runs while nothing is commanded means it is wired to `NC` instead of `NO`, or the
   relay is not in its current path. Stop here until that is fixed — on `NC` the pump
   runs whenever the coil is idle and there is no firmware state that can stop it.
4. Power on and check the board boots and joins Wi-Fi.
5. `curl http://192.168.1.39/status` and confirm:
   - `"model":"fomo:2class"` — the trained model compiled in. `"fomo:stub"`
     means the Edge Impulse export is missing and nothing will ever spray.
   - `"sprayActive":false`.
6. `curl "http://192.168.1.39/spray?duration=3000"`. The relay, the GPIO1 LED and
   the GPIO21 buzzer should all activate for 3 seconds, and the serial log should
   show `Spray ON (3000 ms) relay=14 level=0 (active-low) led=1 buzzer=21` then
   `Spray OFF`. Watch for a reset during the spray — that is the brownout symptom of
   the pump sharing the board's supply.
7. **If the LED and buzzer respond but the relay does not click**, the module is
   active-high. Run `curl -X POST http://192.168.1.39/config -d
   'relay_active_low=0'` (reboots) and repeat step 6. No reflash needed.
8. Note that a manual spray starts the cooldown (`cooldown_ms`, currently 30 s), so do
   it before a real detection test rather than between attempts. A reboot clears it.
9. Finally, show feces to the camera. `/status` `detections` should increment and
   `lastConfidence` should rise above the configured `threshold`.

## Pins to avoid

| Pins | Reason |
|------|--------|
| 0, 3, 45, 46 | strapping pins, affect boot |
| 19, 20 | USB D-/D+ |
| 26-32 | SPI flash |
| 33-37 | octal PSRAM |
| 4-13, 15-18 | camera (see pin table above) |

Free and safe: 1, 2, 14, 21. Of those, 1 / 14 / 21 are taken by the LED / relay /
buzzer, leaving 2 -- which is best avoided anyway, because on many devkits it is an
addressable RGB LED that needs a NeoPixel/RMT bitstream rather than a plain GPIO.

If you ever need another alarm output, do not reuse 1, 14 or 21 -- and never share a
pin with the buzzer.
