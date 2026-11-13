# BNTS Heart Rate

A pulse sensor on an Arduino sends every heartbeat over USB to a Raspberry Pi, and the Pi's 7" touchscreen runs a simple
press-to-start heart rate check in the BNTS colours:

1. **Start screen** - a beating pixel heart. It says to wear the wristband with the sensor pressing on the top of your wrist,
   then to press the screen to begin measuring your heart rate.
2. **Measuring** - after the press the sensor is live. The screen asks for a finger, and when the first beat arrives it
   records **every beat for 10 seconds**: the heart beats with each real beat, the pulse graph scrolls, and a countdown runs.
3. **Your heart rate** - the **average** heart rate with *"Average of N beats over S seconds"*, shown for **10 seconds**.
4. Back to the start screen.

If no pulse is found (no finger, or fewer than 3 beats), the screen says *"No pulse found"* for 5 seconds and starts over.

```
finger -> [Heart rate sensor] -> Arduino (D2) --USB--> Raspberry Pi 3B+ -> 7" touchscreen (Chromium, full-screen)
                                  reports every beat    server.py (Python)    also viewable via Raspberry Pi Connect
```

## No filtering, and how the average is worked out

The Arduino does not filter, judge or tidy the heart rate: **every pulse the sensor produces is a beat**, and the time of each
beat (the Arduino's own millisecond clock) is sent to the Pi. The one thing it does is smooth away the sensor's few-millisecond
flicker at the start and end of each pulse (visible in the sensor's raw output), because counting that raw would give 5-20 "beats"
per heartbeat. That is not about the heart; it just makes each pulse count exactly once.

The Pi then records the beat times for 10 seconds from the first beat and works out the **true average**:

> average BPM = 60,000 x (beats - 1) / (time from the first beat to the last beat, in milliseconds)

- It has none of the "plus or minus one beat" error you get from counting beats in a fixed window (counting in 10 seconds
  can be out by 6 BPM).
- It uses the Arduino's clock, so delays on the USB cable or in the browser cannot affect it.

Tested (`tests/`): each pulse is exactly one beat at every rate from 40 to 200 BPM; the gaps between reported beat times match
the real gaps to under a millisecond; the average is exact wherever in a heartbeat the window starts; and the exact compiled
firmware does the same in an AVR emulator.

> **Note on the sensor:** the part is the **DFRobot Gravity** heart rate sensor (SEN0213), not an Adafruit part.

## What's in this folder

| Path | What it is |
|---|---|
| `arduino/bnts_heartrate/` | The Arduino firmware (`.ino`) and its beat code (`BeatSensor.h` digital, `AnalogBeats.h` analog, `StatusLed.h`) |
| `arduino/sensor_test*/` | Standalone test sketches for checking the sensor from a Mac/PC (see "Test the sensor first") |
| `pi/server.py` | Reads the Arduino's serial port, serves the display |
| `pi/web/` | The display (HTML/CSS/JS, BNTS pixel mark). `measure.js` is the average maths |
| `pi/kiosk.sh` | Launches the display full-screen and keeps it running |
| `pi/update.sh` | Boot-time updater: pulls the latest code from GitHub and installs it |
| `pi/serial-monitor.sh` | The `bnts-monitor` command: the Arduino's serial output over SSH |
| `install.sh` | One-shot Pi setup (service, kiosk, autologin, auto-start) |
| `check-arduino.sh` | Checks the Arduino is connected and sending data the display understands |
| `flash-arduino.sh` | Flash the Arduino **from the Pi** (the Pi can do this) |
| `uninstall.sh` | Undo everything |
| `tests/` | Beat tests, average tests, serial/monitor tests, updater and auto-login tests, and tests that build the firmware for an Uno and run it in an AVR emulator (run on a PC) |

---

## 1. Wire the sensor

The Gravity sensor has a 3-wire cable. Check the labels on the sensor board, then (switch on **D**, the default):

| Sensor | Arduino |
|---|---|
| Signal (S / AOUT) | **D2** (digital pin 2) |
| VCC (+) | **5V** (3.3V also works) |
| GND (-) | **GND** |

(If you ever set the switch to **A**, use **A0** for the signal instead. See "Sensor mode" below.)

Plug the Arduino into the Pi with a **data** USB cable (some cables are charge-only).
Power: the Pi 3B+ driving the display *and* an Arduino wants a good **2.5 A** supply.

### Sensor mode: the A/D switch

The sensor has a small switch for its output mode, and the firmware must match it. At the top of
`arduino/bnts_heartrate/bnts_heartrate.ino`:

| Switch | Setting | What you get |
|---|---|---|
| **D** (digital) | `#define SENSOR_MODE_DIGITAL 1` (the default). Signal on **D2**. | One pulse per heartbeat. The pulse graph on the display is a drawn pulse shape triggered by each real beat (digital mode has no waveform). |
| **A** (analog) | `#define SENSOR_MODE_DIGITAL 0`. Signal on **A0**. | The real pulse waveform on the graph; a beat is each rise above a threshold (`AnalogBeats.h`). |

Change it, then flash again (step 2).

**The Arduino's own LED tells you what it sees.** The little LED on the board (pin 13) flashes briefly with **every beat the
Arduino detects**. Put a finger on the sensor: if the LED flashes in time with your pulse, the Arduino is working. If it never
flashes, the Arduino isn't detecting a pulse: check the wiring, that the signal is on **D2**, the A/D switch, and that the finger
is on the sensor window. If it flashes but the Pi doesn't react, the problem is between the Arduino and the Pi: run
`./check-arduino.sh`.

### Test the sensor first (optional, from a Mac/PC)

Each of these is a standalone sketch for the Arduino IDE. Upload one, then open the Serial Monitor at 115200 baud.

> **After using a test sketch, flash the real firmware again** (`./flash-arduino.sh` on the Pi). The test sketches print
> readable text, which the Pi display can't use: with one loaded, the Arduino *is* transmitting, but the display won't work.
> The display shows "The Arduino isn't running the heart rate program" when it sees that.

| Folder in `arduino/` | Use it to |
|---|---|
| `sensor_test_digital/` | Check the sensor in **digital** mode (older diagnostic: its own 5 s analysis and BPM) |
| `sensor_test_analog_5s/` | Same for **analog** mode, with a warning if the signal looks stuck |
| `sensor_test/` | Live signal readout (or Serial Plotter graph) in analog mode |
| `sensor_test_dfrobot_analog/` | DFRobot's own library in analog mode, plus a signal readout |

(These four have their own older BPM logic. They are only for checking the sensor; the product itself uses none of it.)

## 2. Put the program on the Arduino

**Easiest - from the Pi** (after step 3, with the Arduino plugged into the Pi):

```bash
cd ~/BNTS-Gallery-Projects/heart-rate-monitor
./flash-arduino.sh
```

It installs `arduino-cli`, compiles, pauses the display service, uploads, restarts the service, and then checks the Pi can hear
the Arduino. Needs internet on the Pi the first time. Not an Uno? Pass your board:

```bash
BOARD=arduino:avr:nano:cpu=atmega328old ./flash-arduino.sh    # older Nano clones
BOARD=arduino:avr:nano ./flash-arduino.sh                      # newer Nanos
PORT=/dev/ttyUSB0 ./flash-arduino.sh                           # if auto-detect picks the wrong port
```

**Alternative - from a laptop:** open `arduino/bnts_heartrate/bnts_heartrate.ino` in the Arduino IDE (keep the three `.h` files
beside it), pick your board and port, press Upload.

Sanity check: open the Serial Monitor at **115200** baud. You should see about 50 lines per second like `H,512,0,48213`
(`H,signal,beat,beatMs`). With a finger on the sensor, `beat` becomes 1 once per heartbeat and `beatMs` changes.

## 3. Set up the Raspberry Pi

### 3a. Install

1. Flash **Raspberry Pi OS (with desktop)** (Bookworm or newer) using Raspberry Pi Imager. In the Imager's settings set a
   username, Wi-Fi and enable SSH.
2. Attach the official 7" display (ribbon cable + power as in the Pi docs) and boot.
3. SSH into the Pi and get the code (the repo is public, so no login is needed):
   ```bash
   sudo apt install -y git
   git clone https://github.com/jackallen28/BNTS-Gallery-Projects.git
   cd BNTS-Gallery-Projects/heart-rate-monitor
   ./install.sh --quiet-boot
   sudo reboot
   ```

After the reboot the Pi logs in by itself and the touchscreen shows **only** the heart rate display.

### What "only the display" means here

`install.sh` does the following so nothing else ever appears on the touchscreen:

- Boots straight to the desktop with auto-login (no login screen), set up directly in the login screen's settings.
- Replaces the desktop's autostart (`~/.config/labwc/autostart`) with one that launches **only** the
  kiosk. That means **no taskbar, no desktop icons or wallpaper, no update/notification pop-ups.**
- Runs Chromium in `--kiosk` mode with a fresh profile every launch (no "restore pages?" bubble, no
  password/keyring prompts, no translate bar, no error dialogs), and relaunches it if it ever quits.
- Switches off every touch gesture except the press that starts a measurement (no long-press menus, zoom, scroll,
  text selection) and hides the cursor.
- `--quiet-boot` hides boot text, the Pi logo and the blinking console cursor. (Originals are backed up as
  `*.before-bnts`; `uninstall.sh` restores them.)

> **Caveat:** a *keyboard* plugged into the Pi can still use desktop shortcuts (e.g. Alt+F4 closes the
> browser, though it comes straight back). Touch alone can't get out. (Space or Enter also starts a measurement.)

### 3b. Raspberry Pi Connect (remote viewing)

Pi Connect's **screen sharing** shows the Pi's live desktop session, which is why this setup keeps the
normal Wayland desktop running (with only the kiosk on it) instead of using a bare kiosk compositor.
Requirements: the **full** `rpi-connect` package (not `rpi-connect-lite`, which has no screen sharing), and the
**labwc/Wayland** session (the installer selects it).

```bash
sudo apt install rpi-connect        # skip if already installed
rpi-connect on
rpi-connect signin                  # follow the link it prints, then view at connect.raspberrypi.com
rpi-connect status
```

You'll see exactly what's on the touchscreen (and can click it to start a measurement). If screen sharing doesn't appear after a
reboot, run `rpi-connect doctor`.

## 4. Try it without hardware

On any computer with Python 3 (no Arduino needed):

```bash
cd pi && python3 server.py --simulate        # a fake pulse on the sensor for 30 s, off for 8 s
# then open http://127.0.0.1:8080/ and click the screen   (add ?debug for an on-screen readout)
```

`--simulate on` keeps a finger on permanently, `--simulate off` never has one (to see "No pulse found").

## Settings

Edit `/etc/default/bnts-heart`, then `sudo systemctl restart bnts-heart` (and `sudo reboot` for `ROTATE` / `KIOSK_QUERY`):

- `BNTS_ARGS="--port /dev/ttyACM0"` - pin the serial port (default: auto-detect)
- `ROTATE=180` - rotate the screen if it's mounted upside-down
- `KIOSK_QUERY="?measure=15&result=8"` - change the timings (in seconds). Options: `measure` (10, how long every beat is
  recorded), `result` (10), `wait` (20, how long to wait for a finger after the press), `failed` (5)

A longer `measure` means more beats in the average. The wording is in `pi/web/index.html`, and the look and the process are in
`pi/web/app.js` (settings at the top) and `pi/web/style.css`. To add the real BNTS logo, drop a `logo.svg` into `pi/web/`; it
appears top-left. The pixel mark (`bnts-pixels.svg`) is top-right.

Analog mode only: `ANALOG_MIN_AMPLITUDE` at the top of `AnalogBeats.h` is how tall a pulse must be to count (raise it if an empty
sensor produces phantom beats, lower it for a weak signal).

Tip for good readings: rest a fingertip lightly on the sensor window and hold still. Pressing hard or wiggling ruins the signal.
Strong direct sunlight on the sensor can also swamp it.

## Automatic updates

`install.sh` also sets up `bnts-update.service`, which runs **every time the Pi boots** (reboot, restart or power-up):

1. waits up to about a minute for the internet (Wi-Fi can be slow to connect),
2. pulls the latest code from GitHub into your clone (`git pull --ff-only`),
3. if anything is new, copies it to `/opt/bnts-heart` and restarts the display server.

The screen reloads itself to pick up the new version, but only while it is idle on the start screen, so it never
interrupts a measurement. The display always starts: with no internet, or if the pull fails, the version already installed keeps
running.

- **Check what it did:** `journalctl -u bnts-update -b`
- **Update right now:** `sudo /opt/bnts-heart/update.sh`
- **Turn it off:** set `AUTO_UPDATE="0"` in `/etc/default/bnts-heart`
- **Firmware is never flashed automatically.** If an update changes the Arduino firmware, the log says so; run
  `./flash-arduino.sh` yourself so an Arduino is never reprogrammed by surprise.
- If you change files inside the clone by hand, `git pull` can refuse to overwrite them (the log says "git pull failed").
  Undo your edits with `git -C ~/BNTS-Gallery-Projects checkout -- .` or put your settings in `/etc/default/bnts-heart` instead.
- A change to `install.sh` itself (new packages, new services) needs `./install.sh` run again; the log tells you.

## Watch the Arduino's serial output from an SSH session (Serial Monitor)

```bash
bnts-monitor
```

Shows what the Arduino is sending, like the Arduino IDE's Serial Monitor, without stopping the display: the display service
already has the Arduino's serial port open, so the command asks it for a copy of what it receives. Press Ctrl+C to quit.

| Command | Shows |
|---|---|
| `bnts-monitor` | the Arduino's `# ...` messages, plus one line for **every beat**: when it happened, the gap since the last beat and the rate that works out to |
| `bnts-monitor --all` | every line exactly as sent (about 50 data lines a second) |
| `bnts-monitor --data` | only the beat lines |

```
  beat 1   at 48213 ms   gap 833 ms = 72 BPM
```

If a test sketch is loaded on the Arduino you'll see its text here too, which makes it easy to tell what is on the board.
If the display service isn't running, `bnts-monitor` reads the Arduino's port directly instead. `install.sh` sets up the command
(`/usr/local/bin/bnts-monitor`); until you've run it, use `./pi/serial-monitor.sh`.

## Is the Arduino sending data to the Pi?

```bash
./check-arduino.sh
```

It tells you whether the Arduino is on USB, whether the display service has its port open, and whether data is arriving, and it
names the fix when something is wrong (a test sketch loaded, older firmware, nothing arriving). For the raw lines coming from the
Arduino (the display service is paused for about 6 seconds while it listens):

```bash
./check-arduino.sh --raw
```

A healthy Arduino sends about 50 lines a second that start with `H,`. `./flash-arduino.sh` runs the same check for you after it
uploads and prints OK or a warning.

## Troubleshooting

| What you see | Try |
|---|---|
| Pressed the screen, it says "Place your finger on the sensor" and then "No pulse found" | The Arduino isn't sending beats. Watch the Arduino's LED (flashes with each beat), then `bnts-monitor` and `./check-arduino.sh`. Check the signal wire is on **D2** and the switch is on **D** |
| "Sensor not connected" chip on screen | Check the USB cable (data cable?). `ls /dev/ttyACM* /dev/ttyUSB*` should list a device. `journalctl -u bnts-heart -f` |
| "Waiting for data from the sensor" | Nothing is arriving: run `./check-arduino.sh`, then try another USB cable or `./flash-arduino.sh` |
| "The Arduino isn't running the heart rate program" | A test sketch (or something else) is on the Arduino. Run `./flash-arduino.sh` to put the real firmware back |
| "The Arduino has old firmware" | The Arduino is running a previous version. Run `./flash-arduino.sh` |
| The numbers look odd | Run `bnts-monitor`: each beat line shows the gap in milliseconds. Gaps that are wildly uneven mean the finger is moving or the sensor window is covered badly |
| Flash fails: *permission denied* on the port | Log out and in once (adds you to `dialout`), or `reboot` |
| Flash fails: *not in sync / programmer not responding* | Wrong `BOARD` for your clone: try the `atmega328old` Nano variant |
| Black screen after reboot | `cat /tmp/bnts-kiosk.log` via SSH; confirm `echo $XDG_SESSION_TYPE` is `wayland` in a desktop session |
| A new version didn't appear after a reboot | `journalctl -u bnts-update -b` shows why (no internet, local edits in the clone, or `AUTO_UPDATE="0"`) |
| A blue box saying "Programmer error, unrecognised boot option" during `install.sh` | An older installer called `raspi-config` with option codes your OS doesn't use. Press Enter on <Ok>, then `git pull` and run `./install.sh` again: it now sets up auto-login directly |
| Want the normal desktop back | `./uninstall.sh` then `sudo reboot` |

## How it talks (if you want to change things)

The Arduino sends 50 lines per second at 115200 baud: `H,<signal>,<beat>,<beatMs>`.

- `signal` is the pulse waveform 0-1023 for the graph (in digital mode, a drawn pulse shape after each real beat).
- `beat` is 1 on the line where a beat was just detected.
- `beatMs` is the Arduino's millisecond clock at the most recent beat. It changes with every new beat, and the page works
  out the average from these times (not from when the lines arrive). Lines that start with `#` are human-readable messages.

`server.py` forwards each line to the page as JSON over Server-Sent Events at `/events`, adding the time it read the line. The
page does everything else: a press starts a measurement; the first new `beatMs` starts a 10-second clock; every new `beatMs`
is recorded; at 10 seconds `measure.js` works out the average; the page shows it for 10 seconds,
then waits for the next press. (Lines in the previous firmware's format are recognised, and the screen asks for the new firmware.)

*For fun only. Not a medical device.*
