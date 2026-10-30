# Target protocol — camera board → main ESP32

The wire contract between the **camera ESP32-S3** (Laptop A, this repo) and the
**main ESP32** (Laptop B) that owns the relay, the water pump, the servos and
the water-level sensor.

Both boards are on the same Wi-Fi network, so the camera board talks to the
main board **directly**. The Python YOLOv8 verifier and the CodeIgniter
dashboard are Laptop A's concern and are **not** in this path — the main board
must keep working when Laptop A is off.

The camera board never sends servo angles. It sends image coordinates; the
main board converts them (§5).

---

## 1. Roles

| Board | Owns | Does not own |
|---|---|---|
| Camera ESP32-S3 (Laptop A) | camera, FOMO detection, YOLOv8 verify poll, publishing coordinates, **its own legacy local spray path** | servos, the pen's water pump, any angle calculation |
| Main ESP32 (Laptop B) | 2 servos, pump relay, water-level sensor, coordinate→angle conversion, the cleaning cycle state machine | the camera, detection, any inference |

The camera board's local spray path is **left intact** on purpose — that board
may be reused for other purposes. See §7 for the double-spray hazard that
creates and how to switch the local pump off.

---

## 2. Message: `POST /target`

Request: `Content-Type: application/json`, body as below.

```json
{
  "v": 1,
  "detection_id": 4211,
  "ts_ms": 1761734400000,
  "source": "fomo",
  "frame": { "width": 640, "height": 480 },
  "count": 2,
  "boxes": [
    { "x": 0.6630, "y": 0.0365, "w": 0.1029, "h": 0.1183, "conf": 0.91 },
    { "x": 0.2365, "y": 0.4471, "w": 0.0957, "h": 0.1202, "conf": 0.74 }
  ]
}
```

### Fields

| Field | Type | Meaning |
|---|---|---|
| `v` | int | Protocol version. Currently `1`. A receiver **must** reject a version it does not know rather than guess. |
| `detection_id` | uint32 | Monotonic counter, increments per published detection, per boot. Together with `ts_ms` this is the duplicate guard (§4). |
| `ts_ms` | uint64 | Unix epoch milliseconds at publish time. The main board uses it only for logging and for rejecting implausibly old messages — it must **not** use it to schedule anything, because the two boards' clocks can differ. |
| `source` | string | `"fomo"` or `"yolo"`. Which model produced the boxes. Never omitted. |
| `frame.width` / `frame.height` | uint16 | The image the coordinates refer to. Currently always 640×480 (`FRAMESIZE_VGA`), but it is on the wire so the receiver never hardcodes it. |
| `count` | uint8 | Number of entries in `boxes`. Must equal `boxes.length`. |
| `boxes[].x`, `.y` | float | **Top-left** corner, normalized 0..1 relative to `frame`. Not a centroid. |
| `boxes[].w`, `.h` | float | Size, normalized 0..1. |
| `boxes[].conf` | float | 0..1 confidence for that box. For `source: "fomo"` this is the per-box FOMO score. For `source: "yolo"` it is the **headline** target-class score, repeated on every box: the camera board's verifier parser keeps the best target-class confidence, not a score per box. |

### Coordinate semantics — read this twice

- **Top-left, not centre.** The camera board converts FOMO's centroid to a
  top-left corner before publishing (`http_server_service.c:175-178`), so the
  two detectors' boxes are directly comparable. The verifier's `boxes_norm`
  array is already top-left.
- **Normalized, not pixels.** Both the FOMO boxes and the YOLOv8 boxes are
  normalized to the frame, and the verifier's frame is a *uniform* resize of
  the camera frame (`app.py:170-175` scales width and height by the same
  factor), so the normalized coordinates of the two sources agree. The
  receiver can multiply by `frame.width`/`frame.height` if it wants pixels.
- **The camera board sends the better box it has.** YOLO boxes when the
  verifier answer is fresh, otherwise the local FOMO boxes. `source` says
  which, always.

### Box count

Up to **8** boxes (the camera board's `FOMO_MAX_FECES_BOXES`); the verifier
supplies up to **4** (`VERIFY_MAX_FECES_BOXES`). A receiver must accept any
count from 1 to 8 and must not assume a fixed length.

---

## 3. Reply

The main board answers with JSON:

```json
{ "ok": true, "accepted": true,  "pan": 42.0, "tilt": 18.0, "reason": "" }
{ "ok": true, "accepted": false, "pan": 0.0,  "tilt": 0.0,  "reason": "low_water" }
{ "ok": false, "error": "bad_version" }
```

| Field | Meaning |
|---|---|
| `ok` | The request was understood. `false` means the message was malformed or the version was unknown — the camera board will not retry a malformed message. |
| `accepted` | The cleaning cycle was started (or would be, once the servos finish moving). `false` means it was **refused**; `reason` says why. |
| `pan`, `tilt` | The servo angles the main board computed for the **best** box in the message, in degrees. Echoed so the operator can see where the nozzle was aimed, even when the cycle was refused. |
| `reason` | One of the codes in §5. Empty when `accepted` is true. |
| `error` | Present only when `ok` is false. |

The camera board records the last reply and shows it in `/status` and on the
dashboard, so "sent but refused" is distinguishable from "never arrived".

---

## 4. Duplicate suppression

The camera board publishes on a **detection**, and one frame can hold several
spots. Two rules keep the main board from cleaning the same spot twice:

1. **`detection_id`** is monotonic per boot. The main board keeps the highest
   id it has seen and **ignores any message whose id is not greater**. A
   reboot resets the camera board's counter, so the main board must also
   accept a *lower* id when it has not seen one for a while — see §5
   `stale_id`. The simplest rule that works: ignore a message whose id equals
   the last accepted id, and otherwise accept it.
2. **`ts_ms`** lets the main board reject a message that is obviously stale
   (older than `MAX_TARGET_AGE_MS`, default 30 s). Clocks can differ between
   the boards, so this is a sanity bound, not a scheduling input.

Neither rule replaces the main board's own cycle guard: a message that is
genuinely new still cannot start a second cycle while one is running.

---

## 5. Safety matrix

Every refusal has exactly one reason code. The main board **must not** start a
cycle in any of these cases, and must not move the servos for a refused
message either.

| Reason | Condition | Notes |
|---|---|---|
| `disabled` | The operator has switched the system off. | Detection is still logged. |
| `low_water` | Water level is below the configured minimum. | Re-checked **during** the cycle, so a tank that empties mid-spray stops the pump. |
| `sensor_invalid` | The water reading is out of range or the sensor is disconnected. | Treated as *not enough water*: fail closed. |
| `busy` | A cleaning cycle is already running. | The message is dropped, not queued. |
| `duplicate` | The detection id was already handled. | Silent. |
| `stale_id` | The message is older than `MAX_TARGET_AGE_MS`. | Clock-skew guard. |
| `no_target` | The message contained no usable box. | Malformed message; the camera board should not send one. |
| `out_of_range` | The computed angles are outside the servo's limits. | The main board clamps to the limit instead, but says so here. |

### The interlock order

Checks run **in this order**, and the first failure wins:

1. Protocol version known? → else `ok:false`
2. Message well-formed and at least one usable box? → else `no_target`
3. System enabled? → else `disabled`
4. Water reading valid? → else `sensor_invalid`
5. Water level sufficient? → else `low_water`
6. No cycle running? → else `busy`
7. Duplicate / stale id? → else `duplicate` / `stale_id`

Water is checked **before** the cycle guard so a dry tank is reported as
`low_water` rather than `busy` — the operator needs to know the tank is the
problem.

---

## 6. Angle conversion (main board only)

The main board converts image coordinates to servo angles. It must not treat a
pixel value as an angle.

```
u,v          normalized pixel (top-left of the box) -> pixel centre:
             px = (x + w/2) * frame.width
             py = (y + h/2) * frame.height

ray          camera-frame direction:
             dx = (px - cx_px) / f
             dy = (py - cy_px) / f
             dz = 1
             where f is the focal length in pixels (or derived from the FOV)

world        rotate (dx, dy, dz) by the camera's yaw and pitch into world
             axes, with +Z up and the floor at z = 0

floor        t = cam_height / (-ray.z)        (ray must point downward)
             P  = cam_pos + t * ray            -> world (X, Y) on the floor

nozzle       pan  = atan2(X - nozzle_x, Y - nozzle_y)
             tilt = atan2(nozzle_height, hypot(X - nozzle_x, Y - nozzle_y))

limits       clamp pan/tilt to the servo's mechanical limits, then map the
             angle to a pulse width.
```

The camera's principal point defaults to the frame centre; the focal length
can be given directly or derived from a horizontal field of view.

All of these values are calibration, stored in the main board's NVS and
editable over `POST /config`:

`cam_x`, `cam_y`, `cam_height`, `cam_yaw_deg`, `cam_pitch_deg`, `focal_px`
(or `hfov_deg`), `nozzle_x`, `nozzle_y`, `nozzle_height`, `pan_min`, `pan_max`,
`tilt_min`, `tilt_max`, `water_min_pct`.

### 6.1 The pen model — the model this rig uses

The camera looks **straight down** at the pen, so the pen appears in the
picture as a rectangle and every centimetre is the same size as every other
one. The picture **is** the pen, and the mapping needs nothing but the pen's
size:

```
pen frame     X = across the pen, cm, 0 at the near-left corner as the
              camera sees it
              Y = along the pen, cm, 0 at the camera's end

picture       u = X / width                 (the picture's left is the pen's left)
              v = 1 - Y / length            (the picture's BOTTOM is the pen's
                                             near end; flip if the camera is
                                             mounted the other way round)
```

Written as a homography — which is what the code stores and applies — that is

```
H = [ 1/W,   0,    0,
      0,   -1/L,   1,
      0,    0,    0 ]
```

which is affine, so the same code that fits a perspective handles it with no
special case anywhere downstream.

**When the camera is not square-on**, the picture is a trapezoid and one scale
factor cannot describe it. Then a **homography fitted to the picture** is used
instead: four point correspondences (the pen's four corners) fit it exactly,
and it is stored in the same eight numbers. The dashboard's live view fits it —
**Fine-tune the pen** — and pushes it to the board:

```
pen_ok        1 to use the pen model
pen_w, pen_l  the pen's size in cm
pen_flip_y    1 when the camera is at the top of the picture
pen_h         the eight numbers, comma separated -- only when the picture has
              been fitted; without it the board uses the plain model
```

The board's own `pen_nx`, `pen_ny`, `pen_nh`, `pen_po`, `pen_to`, `pen_pd`,
`pen_td`, `pen_pmin`, `pen_pmax`, `pen_tmin`, `pen_tmax` are the physical
facts of the rig, and stay where the operator set them. A matrix that cannot
be inverted — four points in a line — is refused rather than stored.

The nozzle faces **back** toward the camera, so its forward direction is −Y,
and the aim is computed in the pen frame:

```
fwd = nozzle_y - Y
lat = X - nozzle_x
tilt = tilt_offset + tilt_dir * atan2(nozzle_height, 10*hypot(lat, fwd))
pan  = pan_offset - pan_dir * atan2(lat, fwd)
```

clamped to the servo's travel in the servo's own numbering.

---

## 7. Commissioning note — the double-spray hazard

The camera board keeps its own relay and its own local spray path (it is left
unchanged so it can be reused for something else). While the main board is
also connected, **both** would spray for one detection.

Before going live with Laptop B, switch the camera board's local pump off:

```bash
curl -X POST http://192.168.254.50/config -d 'spray_enabled=0'
```

The camera board then still detects, still alerts, still logs and still
publishes coordinates — it just refuses to energise its own relay. The
`/setup` page has the same switch. This is the existing operator kill switch;
no new firmware is involved.

---

## 8. Ports and addressing

| Thing | Default |
|---|---|
| Main ESP32 HTTP port | `8080` |
| Target endpoint | `POST /target` |
| Main board status | `GET /status` |
| Main board manual servo | `GET /servo?pan=<deg>&tilt=<deg>` |
| Main board manual pump | `GET /pump?duration=<ms>` |
| Camera board setting | `target_url` = `http://<main-ip>:8080/target` |

`target_url` is stored in the camera board's NVS and is settable over
`POST /config` or the `/setup` page — no reflash to point it at a different
main board. `target_enabled` switches publishing off entirely.
