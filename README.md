# ESP32-S3 Face Cam - PlatformIO

## 1) Project Summary

This project runs on an ESP32-S3 camera board and does three things at the same time:

1. Captures camera frames.
2. Runs face detection.
3. Streams live video on a web page over ESP32 SoftAP (Wi-Fi access point).

It also sends face position data over I2C and prints face data in serial logs.

This README explains the full workflow and all important technical details in simple English.

---

## 2) Final Features Implemented

### 2.1 Build and include fixes

- Fixed header include typo:
  - `camera_setting.h` -> `camera_settings.h`
- Result: project builds correctly.

### 2.2 Sensor detection and sensor name API

- Added PID-to-name mapping for known sensors.
- Added reusable API:
  - `const char *get_camera_sensor_name(void)`
- Added safe NULL handling if `esp_camera_sensor_get()` fails.
- Added sensor detection logs:
  - `ESP_LOGI(...)`
  - `printf("[CAM] Detected camera sensor: ...")`

Known PIDs mapped:

- OV2640: `0x2640`
- OV3660: `0x3660`
- OV5640: `0x5640`
- GC2145: `0x2145`
- GC0308: `0x0308`
- GC032A: `0x032A`

### 2.3 Sensor orientation update for GC2145

- Added GC2145 tuning branch in camera setup:
  - `set_vflip(...)`
  - `set_hmirror(...)`
- Reason: your board reported GC2145, so the old block was incomplete.

### 2.4 AP web streaming

- ESP32 runs as Wi-Fi AP:
  - SSID: `ESP32S3-FACE-CAM`
  - Password: `12345678`
- Web server started on port 80.
- Browser page at `http://192.168.4.1`
- MJPEG stream endpoint at `/stream`.

### 2.5 Queue method restored, tuned for low latency

You asked to move back from Task Notification to Queue method. That is now done.

Current design is queue-based and optimized for "latest frame" behavior:

- Queue length is `1` for AI frame queue and stream frame queue.
- Non-blocking send/receive is used for these queues.
- Old frame is dropped and returned when a new frame arrives.
- This reduces lag and keeps stream closer to real-time camera movement.

### 2.6 Camera grab mode tuned for responsiveness

- Camera grab mode is set to:
  - `CAMERA_GRAB_LATEST`
- This also helps reduce delayed/stale frames.

### 2.7 Stream black-screen mitigation

In stream collector task, JPEG conversion now uses fallback logic:

1. Try `frame2jpg(...)`
2. If fail, fallback to `fmt2jpg(...)`

Also added periodic debug log when conversion fails repeatedly.

### 2.8 Logging level update

- Added to `platformio.ini`:
  - `-DCORE_DEBUG_LEVEL=3`
- This enables visible INFO logs (including camera info).

---

## 3) High-Level Architecture

## 3.1 Main runtime tasks

### A) Camera producer task

- Gets raw frame from camera driver (`esp_camera_fb_get()`)
- Publishes to AI queue (length 1, latest-only behavior)

### B) Face detection task

- Receives frame from AI queue
- Runs two-stage face detection:
  - `HumanFaceDetectMSR01`
  - `HumanFaceDetectMNP01`
- Draws detection boxes/landmarks on frame
- Logs result like:
  - `center_x:..., center_y:..., width:..., length:...`
  - or `[FACE] none`
- Forwards frame to stream queue (length 1, latest-only behavior)
- Sends compact result to I2C queue

### C) Stream collector task

- Receives latest processed frame from stream queue
- Converts frame to JPEG
  - first `frame2jpg`, fallback `fmt2jpg`
- Stores latest JPEG in shared memory buffer protected by mutex

### D) Web server task

- Handles `/` and `/stream`
- `/` serves HTML page
- `/stream` serves MJPEG multipart stream

### E) I2C send task

- Waits for detection result queue
- Responds to I2C master request with face data bytes

---

## 4) Data Flow (End-to-End)

1. Camera captures frame.
2. Frame goes to AI queue (latest frame kept).
3. Face detection consumes frame.
4. Detection result is generated.
5. Same frame goes to stream queue (latest frame kept).
6. Stream collector converts to JPEG and updates shared JPEG buffer.
7. Browser reads `/stream` and displays latest JPEG frames continuously.
8. I2C task uses detection output for external controller.

---

## 5) Why This Queue Strategy Was Chosen

You reported slow stream and wanted better responsiveness when physically moving camera.

The current strategy improves "live feel":

- It prefers newest frame over old frame.
- It drops stale frames under load.
- It reduces visual delay (latency).

Trade-off:

- Some frames are skipped when CPU is busy.
- But stream stays closer to current camera motion.

This is usually better for live monitoring.

---

## 6) Build and Run Commands

Use the local PlatformIO binary in this environment:

```bash
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run --target upload
~/.platformio/penv/bin/pio device monitor --baud 115200
```

If monitor blocks upload (port busy):

```bash
lsof /dev/ttyUSB0 | awk 'NR>1 {print $2}' | xargs -r kill
```

Then upload again.

---

## 7) Web Access

1. Power the board.
2. Connect phone/laptop Wi-Fi to:
   - SSID: `ESP32S3-FACE-CAM`
   - Password: `12345678`
3. Open browser:
   - `http://192.168.4.1`

---

## 8) Serial Output You Should See

Typical boot/run logs:

- `[CAM] Detected camera sensor: GC2145 (PID: 0x2145)`
- `[AP] SSID:... PASS:... IP:192.168.4.1`
- `center_x:..., center_y:..., width:..., length:...`
- `[FACE] none`

If stream conversion is failing repeatedly, you may also see:

- `[STREAM] JPEG conversion failed count=... format=... w=... h=...`

---

## 9) Troubleshooting

### 9.1 Stream is black

Possible causes:

- JPEG conversion failure
- Browser cached bad stream state
- Wi-Fi weak signal

Actions:

1. Refresh page or reopen browser tab.
2. Reboot board and reconnect AP.
3. Check serial monitor for `[STREAM]` failure logs.

### 9.2 Garbled bytes at boot (`<0x80>` etc.)

- Usually serial sync noise near reset.
- If readable logs appear after that, firmware is okay.

### 9.3 Sensor name not printed

1. Confirm `CORE_DEBUG_LEVEL=3` in `platformio.ini`.
2. Press reset after opening monitor.
3. Check for `[CAM] Detected camera sensor...` line.

### 9.4 Upload fails with port busy

- Another monitor process owns `/dev/ttyUSB0`.
- Kill blocking process and upload again.

---

## 10) Technical Change Log (What Was Done)

1. Fixed header include typo causing compile failure.
2. Added AP web stream feature.
3. Added sensor-name function and PID mapping.
4. Added GC2145 sensor orientation handling.
5. Temporarily switched to Task Notification + Mutex.
6. Reverted back to Queue method based on performance preference.
7. Tuned queue pipeline to latest-frame, non-blocking behavior.
8. Set camera grab mode to `CAMERA_GRAB_LATEST`.
9. Added stream JPEG conversion fallback and diagnostics.
10. Enabled INFO logging with `CORE_DEBUG_LEVEL=3`.

---

## 11) Current Status

- Build: passing
- Upload: passing
- Sensor detect: working
- Face detection logs: working
- AP stream: implemented
- Low-latency queue mode: implemented

---

## 12) Next Recommended Improvements (Optional)

1. Add FPS and frame-drop counters in serial logs.
2. Add small status JSON endpoint (`/status`) for web diagnostics.
3. Add dynamic stream quality/frame-size controls from web UI.
4. Separate "AI profile" and "Stream profile" build flags.

