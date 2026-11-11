# BNTS Heart Rate

An Arduino reads a pulse sensor and streams it over USB to a Raspberry Pi. The Pi's touchscreen shows
your **live BPM**, a **scrolling pulse graph** and a pixel-bar **history**, all in the BNTS colours. When
nobody's touching the sensor it switches to a **standby screen** (beating pixel heart, rippling pixel
grid, sweeping ECG line, simple rotating prompts, including a "do 20 star jumps, then check again" challenge).

```
 finger → [Heart rate sensor] → Arduino (A0) ──USB──► Raspberry Pi 3B+ ──► 7" touchscreen (Chromium, full-screen)
                                  beat detection         server.py (Python)       also viewable via Raspberry Pi Connect
```

> **Note on the sensor:** the link you sent is the **DFRobot Gravity** analog heart rate sensor (SEN0213),
> not an Adafruit part. It outputs an analog voltage, which is what this firmware expects.

## What's in this folder

| Path | What it is |
|---|---|
| `arduino/bnts_heartrate/` | The Arduino firmware (`.ino`) + beat-detection code (`HeartDetector.h`) |
| `arduino/sensor_test*/` | Standalone test sketches for checking the sensor from a Mac/PC (see "Test the sensor first") |
| `pi/server.py` | Reads the Arduino's serial port, serves the display |
| `pi/web/` | The display itself (HTML/CSS/JS, BNTS pixel mark) |
| `pi/kiosk.sh` | Launches the display full-screen and keeps it running |
| `install.sh` | One-shot Pi setup (service, kiosk, autologin, auto-start) |
| `flash-arduino.sh` | Flash the Arduino **from the Pi** (the Pi can do this) |
| `uninstall.sh` | Undo everything |
| `tests/` | Detector unit test + serial end-to-end test (run on a PC) |

---

## 1. Wire the sensor

The Gravity sensor has a 3-wire cable. Check the labels on the sensor board, then:

| Sensor | Arduino |
|---|---|
| Signal (S / AOUT) | **A0** |
| VCC (+) | **5V** (3.3V also works) |
| GND (−) | **GND** |

Plug the Arduino into the Pi with a **data** USB cable (some cables are charge-only).
Power: the Pi 3B+ driving the display *and* an Arduino wants a good **2.5 A** supply.

### Sensor mode: the A/D switch

The sensor has a small switch for its output mode, and the firmware must match it. At the top of
`arduino/bnts_heartrate/bnts_heartrate.ino`:

| Switch | Setting | What you get |
|---|---|---|
| **D** (digital) | `#define SENSOR_MODE_DIGITAL 1` (the default) | One pulse per heartbeat. The BPM number is real. The pulse graph on the display is a drawn pulse shape triggered by each real beat (digital mode has no waveform). |
| **A** (analog) | `#define SENSOR_MODE_DIGITAL 0` | The real pulse waveform on the graph. |

Change it, then flash again (step 2).

### Test the sensor first (optional, from a Mac/PC)

Each of these is a standalone sketch for the Arduino IDE. Upload one, then open the Serial Monitor at 115200 baud.

| Folder in `arduino/` | Use it to |
|---|---|
| `sensor_test_digital/` | Check the sensor in **digital** mode: finger detected, 5 s analysis, BPM, 5 s pause, repeat |
| `sensor_test_analog_5s/` | Same cycle for **analog** mode, with a warning if the signal looks stuck |
| `sensor_test/` | Live signal readout (or Serial Plotter graph) in analog mode |
| `sensor_test_dfrobot_analog/` | DFRobot's own library in analog mode, plus a signal readout |

## 2. Put the program on the Arduino

**Easiest – from the Pi** (do this after step 3a below, with the Arduino plugged into the Pi):

```bash
cd ~/bnts-heart
./flash-arduino.sh
```

It installs `arduino-cli`, compiles, pauses the display service, uploads, and restarts the service.
Needs internet on the Pi the first time. Not an Uno? Pass your board:

```bash
BOARD=arduino:avr:nano:cpu=atmega328old ./flash-arduino.sh    # older Nano clones
BOARD=arduino:avr:nano ./flash-arduino.sh                      # newer Nanos
PORT=/dev/ttyUSB0 ./flash-arduino.sh                           # if auto-detect picks the wrong port
```

**Alternative – from a laptop:** open `arduino/bnts_heartrate/bnts_heartrate.ino` in the Arduino IDE
(keep `HeartDetector.h` beside it), pick your board and port, press Upload.

Sanity check: open the Serial Monitor at **115200** baud. You should see ~50 lines per second like
`H,512,0,0,6`. With a finger on the sensor the 4th number (amplitude) jumps to 30+ and after a few
beats the 2nd number becomes your BPM.

## 3. Set up the Raspberry Pi

### 3a. Install

1. Flash **Raspberry Pi OS (with desktop)** (Bookworm or newer) using Raspberry Pi Imager. In the
   Imager's settings set a username, Wi-Fi and enable SSH.
2. Attach the official 7" display (ribbon cable + power as in the Pi docs), boot, and get this folder onto the Pi:
   ```bash
   scp -r bnts-heart pi@raspberrypi.local:~/        # from your computer; or copy it with a USB stick
   ```
3. On the Pi (SSH or terminal):
   ```bash
   cd ~/bnts-heart
   ./install.sh --quiet-boot
   sudo reboot
   ```

After the reboot the Pi logs in by itself and the touchscreen shows **only** the heart rate display.

### What "only the display" means here

`install.sh` does the following so nothing else ever appears on the touchscreen:

- Boots straight to the desktop with auto-login (no login screen).
- Replaces the desktop's autostart (`~/.config/labwc/autostart`) with one that launches **only** the
  kiosk. That means **no taskbar, no desktop icons or wallpaper, no update/notification pop-ups.**
- Runs Chromium in `--kiosk` mode with a fresh profile every launch (no "restore pages?" bubble, no
  password/keyring prompts, no translate bar, no error dialogs), and relaunches it if it ever quits.
- Makes the page ignore touches (no long-press menus, zoom, scroll, text selection) and hides the cursor.
- `--quiet-boot` hides boot text, the Pi logo and the blinking console cursor. (Originals are backed up as
  `*.before-bnts`; `uninstall.sh` restores them.)

> **Caveat:** a *keyboard* plugged into the Pi can still use desktop shortcuts (e.g. Alt+F4 closes the
> browser, though it comes straight back). Touch alone can't get out.

### 3b. Raspberry Pi Connect (remote viewing)

Pi Connect's **screen sharing** shows the Pi's live desktop session, which is why this setup keeps the
normal Wayland desktop running (with only the kiosk on it) instead of using a bare kiosk compositor.
Requirements: the **full** `rpi-connect` package (not `rpi-connect-lite`, which has no screen sharing), and the
**labwc/Wayland** session (the installer switches to it with `raspi-config`).

```bash
sudo apt install rpi-connect        # skip if already installed
rpi-connect on
rpi-connect signin                  # follow the link it prints, then view at connect.raspberrypi.com
rpi-connect status
```

You'll see exactly what's on the touchscreen. The taskbar tray icon isn't there any more, so use the commands
above rather than the icon. If screen sharing doesn't appear after a reboot, run `rpi-connect doctor`.

## 4. Try it without hardware

On any computer with Python 3 (no Arduino needed):

```bash
cd pi && python3 server.py --simulate        # cycles: ~8 s standby, ~30 s of a fake pulse
# then open http://127.0.0.1:8080/   (add ?debug for an on-screen readout)
```

`--simulate on` keeps a finger on permanently, `--simulate off` keeps standby.

## Settings

Edit `/etc/default/bnts-heart`, then `sudo systemctl restart bnts-heart` (and `sudo reboot` for `ROTATE`):

- `BNTS_ARGS="--port /dev/ttyACM0"` — pin the serial port (default: auto-detect)
- `ROTATE=180` — rotate the screen if it's mounted upside-down

Detection tuning is at the top of `arduino/bnts_heartrate/HeartDetector.h`:

| Symptom | Change |
|---|---|
| Phantom BPM with nothing on the sensor (analog mode) | raise `MIN_AMPLITUDE` (try 30–40) |
| Never detects a finger / weak signal (analog mode) | lower `MIN_AMPLITUDE` (try 12–15) |
| BPM roughly double the real value | raise `MIN_IBI_MS` / lower `FILTER_ALPHA` |
| Standby takes too long to return | lower `NO_BEAT_TIMEOUT_MS` |

The text, timings and look of the display are in `pi/web/app.js` (`MESSAGES`, the constants at the top) and
`pi/web/style.css`. To add the real BNTS logo, drop a `logo.svg` into `pi/web/` and re-run `install.sh`; it
appears top-left. The pixel mark (`bnts-pixels.svg`) is top-right.

Tip for good readings: rest a fingertip lightly on the sensor window and hold still. Pressing hard
or wiggling ruins the signal. Strong direct sunlight on the sensor can also swamp it.

## Troubleshooting

| What you see | Try |
|---|---|
| "Sensor not connected" chip on screen | Check the USB cable (data cable?). `ls /dev/ttyACM* /dev/ttyUSB*` should list a device. `journalctl -u bnts-heart -f` |
| "Waiting for data from the sensor" | Arduino isn't running the sketch or wrong baud; re-run `./flash-arduino.sh` |
| Flash fails: *permission denied* on the port | Log out and in once (adds you to `dialout`), or `reboot` |
| Flash fails: *not in sync / programmer not responding* | Wrong `BOARD` for your clone — try the `atmega328old` Nano variant |
| Black screen after reboot | `cat /tmp/bnts-kiosk.log` via SSH; confirm `echo $XDG_SESSION_TYPE` is `wayland` in a desktop session |
| Want the normal desktop back | `./uninstall.sh` then `sudo reboot` |

## How it talks (if you want to change things)

The Arduino sends 50 lines/second at 115200 baud: `H,<signal>,<bpm>,<beat>,<amplitude>`.
(In digital mode `signal` is the drawn pulse shape and `amplitude` is 100 once beats are heard.)
`server.py` forwards them to the page as JSON over Server-Sent Events at `/events`.
The page switches to LIVE after 3 consecutive valid BPMs and back to standby 1.5 s after they stop.

*For fun only. Not a medical device.*
