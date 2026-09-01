# Pigpen Feces Detector — Web Dashboard (CodeIgniter 3)

CodeIgniter 3 + MySQL web dashboard for the hybrid ESP32-S3 / YOLOv8 system.
It monitors detections, shows the live ESP32-S3 MJPEG stream, allows manual
spray override, and displays YOLOv8 verification results.

The **old raw-PHP implementation** is preserved for reference under
`legacy_php/`.

## Setup

1. **Install the framework core.** CodeIgniter 3 requires its `system/`
   directory. Download **CodeIgniter 3 (3.1.x)** from
   https://codeigniter.com/download and extract the `system/` folder here:

   ```
   dashboard/
   ├── index.php          <- already provided
   ├── .htaccess          <- already provided
   ├── system/            <- ADD THIS (download)
   └── application/       <- this repo's MVC app
   ```

2. **Create the database.** Import `database.sql` (phpMyAdmin or
   `mysql -u root < database.sql`). This creates `feces_detector` with
   `detections` and `settings` tables (includes YOLOv8 verification columns).

3. **Check `application/config/database.php`** matches your MySQL credentials
   (XAMPP default: user `root`, empty password).

4. **Configure URLs** in `application/config/config.php` (`base_url`).

5. Your XAMPP `htdocs` layout should be:
   `http://localhost/feces-detector-ai/dashboard/`

## Structure

```
application/
├── config/          database, config, routes, autoload
├── controllers/     Dashboard, Live, History, Control, Api
├── models/          Detection_model, Setting_model
├── core/            MY_Controller (URL + HTTP helpers to ESP32 & verify server)
└── views/           dashboard/, live/, history/, control/, partials/
assets/
├── css/style.css    (reused from the original)
└── js/main.js       status polling + dashboard refresh
```

## Endpoints (MVC routes)

| Route | Purpose |
|---|---|
| `/dashboard` | Stats, latest detection, recent activity, settings |
| `/live` | MJPEG stream + status chips |
| `/history` | Paginated, filterable detection log (with YOLOv8-verified flag) |
| `/control` | Manual spray + settings (detector IP/port, verify URL, threshold) |
| `/api/latest` | JSON for dashboard auto-refresh |
| `/api/insert` | JSON POST target for the ESP32-S3 detection reports |
| `/api/status` | Proxies the ESP32-S3 `/status` to the browser |

## Verify server

The settings include a `verify_url` pointing at the **Python YOLOv8 server**
(`server/yolo_verify/`). Start it before relying on verification:
`uvicorn app:app --host 0.0.0.0 --port 8000`.

## ESP32-S3 target

The `esp32_ip` / `esp_port` settings point at the ESP32-S3 board. The ESP32
serves `/stream`, `/status`, and `/spray` as described in the firmware README.
