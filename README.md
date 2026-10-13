<!-- Generated from the monorepo by the publish tool. Edit the monorepo, not this file. -->
> **Safety:** this firmware drives a UV-C lamp, a heater and an atomizer. Read [DISCLAIMER](DISCLAIMER) before flashing. Use it at your own risk.
>
> This is a published mirror; see [CONTRIBUTING.md](CONTRIBUTING.md) for how pull requests are handled. Security reports: [SECURITY.md](SECURITY.md). License: GPL-3.0-or-later ([LICENSE](LICENSE), per-file details in [REUSE.toml](REUSE.toml)).

# ESPHome firmware for the TS-01

The main-board firmware replaces the stock firmware on the main control board. It runs the grow loop on the chip itself (humidity, staged heating, fresh-air exchange and the light schedule), so it needs no cloud. Apps and Home Assistant talk to it over the LAN; the API is described in [`../docs/device-api.md`](docs/device-api.md).

**Status:** the firmware compiles, but nobody has flashed it to a unit yet. Expect to adjust things during the first bench session.

## Builds
| File | What it is |
|---|---|
| `common/terrashroom-core.yaml` | The controller, interlocks and entities. Shared by every build, never compiled on its own. |
| `terrashroom.yaml` | **Owner's build.** Core + Wi-Fi, API key, OTA password and hotspot password from `secrets.yaml`, compiled into the image. Never publish it. |
| `terrashroom-public.yaml` | **Public build** (what automyco.com hands out). Core + no credentials at all: each chamber is set up on first boot. Needs no `secrets.yaml`. See [Public build](#public-build). |
| `common/camera-link.yaml` | The camera link (GPIO16/17) and the camera firmware install (`components/camera_link`). Included by both main-board builds. See `../docs/camera-install.md`. |
| `camera-public.yaml` | **Public camera build** (published as `board: camera`). The chamber installs it over the stock camera's own updater, from the iOS app, with no case opening. No credentials: the camera gets Wi-Fi from the chamber, and the app claims its login. |
| `camera.yaml` | **Owner's camera build.** Wi-Fi, keys, login and S3 frame uploads from `secrets.yaml`. Never publish it. Both camera builds share `common/camera-core.yaml`. |
| `camera_uart_test.yaml`, `camera_backup_bridge.yaml` | Bench builds that include `terrashroom.yaml` unchanged. |

Both main-board builds include the core file unchanged, so the safety interlocks and UV-C behaviour are identical by construction. Only the network layer (`api`, `ota`, `wifi`, `captive_portal` and the provisioning parts) differs.

## Built-in safety
- **UV-C (GPIO18):** OFF at every boot. It turns on only when a person switches **UV-C Lamp** on, and switches itself off after **UV-C Auto-Off** minutes (1–60, default 15), on low water, when an OTA upload starts and before the self-test. Turning it on is refused while Low Water is on. With the opt-in lid reed (`common/lid-interlock.yaml`) it is also refused while the lid is open and cut the moment it opens. See `docs/uvc-bench-test.md`.
- **Heater:** switches off if the HDC1080 reading is missing, more than 60 s old, or above 88 °F. This applies in manual mode too.
- **Atomizer:** locks out after 15 s of dry water-probe readings.
- **Manual control:** touching any actuator turns off Climate Control, so the automation never fights the user.

## Network trust model
- **Control is unauthenticated on your LAN.** The `web_server` REST/SSE API that the iOS app uses has no login, so anyone on your Wi-Fi can change setpoints and run the fans, atomizer or heater. The interlocks above still apply to every request, including the `UV-C Lamp` switch's water interlock and time limit (an accepted risk, see `docs/device-api.md`). Put the chamber on a trusted network or an IoT VLAN. Bringing back auth (HTTP basic + app support) is on the roadmap.
- **Firmware upload over HTTP is disabled** (`web_server: ota: false`). Verified in the build: `USE_WEBSERVER_OTA_DISABLED` is set. `/update` is accepted **only** while the captive portal is active, which happens only when the chamber can't join Wi-Fi and raises its own `AutoMyco Setup` hotspot. That hotspot is protected by `fallback_ap_password`. The public build has no hotspot or captive portal, so `/update` isn't served there at all.
- **Normal updates** go through ESPHome's native OTA (`esphome run`), protected by `ota_password`. The Home Assistant API is encrypted with `api_encryption_key`.

## Build
```sh
uv tool install esphome                # 2026.9.1 or newer: the public build needs `provisioning:`
cp secrets.example.yaml secrets.yaml   # fill in your Wi-Fi details; generate the keys as the comments show
esphome compile terrashroom.yaml
esphome compile terrashroom-public.yaml   # no secrets.yaml needed
```
Images land in `.esphome/build/<terrashroom|terrashroom-public>/build/`: `firmware.factory.bin` (bootloader + partitions + app, written at 0x0, for first installs) and `firmware.ota.bin` (app only, for updates).

## Public build
`terrashroom-public.yaml` is meant for strangers' chambers. Nothing in the image is a secret, and nothing in it is the same on every chamber: anyone can run `strings` on the published file and learn nothing that unlocks someone else's unit. Each chamber gets its credentials on first boot. This relies on features new in ESPHome 2026.9 (runtime API keys that also authenticate OTA, and the `provisioning:` window).

**Getting a new chamber onto Wi-Fi.** There's no setup hotspot. An open one let anyone in radio range point an unclaimed chamber at their own network and claim it there. Both remaining routes need someone physically at the chamber:
1. **Serial (Improv), the normal path:** right after installing, the automyco.com flasher asks for the network name and password and sends them over the same cable. The chamber reports back once it has joined, along with its address. Serial Improv works any time the cable is connected.
2. **Bluetooth (Improv), for changing networks later:** use the Home Assistant app or improv-wifi.com (Chrome, which has Web Bluetooth; iPhone Safari doesn't), and **press the rear button once, briefly,** when it asks to authorise. The press authorises BLE setup for 2 minutes. A short press also toggles the grow light, which is harmless; a 5 s hold runs the self-test instead (which turns Climate Control off), so don't hold it. Without the press, the phone sits at "authorization required" and nothing happens: on 2026-10-04 the owner's first BLE Improv attempt after a Bluetooth install failed exactly like this. BLE Improv runs on a chamber with no Wi-Fi saved, and comes back after 90 s without a connection (e.g. a new router), but only inside the claim window. If the window has closed, power-cycle the chamber first. Since 1.2.2 the same goes for a chamber that has already been on Wi-Fi since it was powered on: to move it to a new network, power-cycle it (next paragraph).

**Bluetooth runs only while Improv needs it (1.2.2).** Public 1.2.0 kept Bluedroid and the BT controller running all the time, just for Improv. On the bench (2026-10-11) that left 29,952 B free, Lowest Free Heap 120 B, and no 48 KB block for a TLS handshake. Every HTTPS feature failed before sending anything: the update check, recipes and remote access. Now `esp32_ble` has `enable_on_boot: false`, and `ble_release.h` (decisions in `common/ble_lifecycle.h`, host-tested) does this:
- **Start:** the stack starts only when ESPHome's Improv asks for it. That means no network is saved at power-on, or the saved one has failed for 90 s while the claim window is open. A chamber that already has Wi-Fi never starts Bluetooth.
- **Stop:** once Wi-Fi has been connected for 30 s with Improv idle (or the claim window has closed), the stack is torn down and `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` hands the controller's reserved DRAM to the heap. That's about 41 KB if the stack never ran, or 26 KB after it did (ESPHome hands back the Classic part when it starts the stack).
- **Until power-on:** Bluetooth can't start again until the next power-on, which is already the only time the claim window reopens. Serial Improv is unaffected.
- **Logs:** the log says when Bluetooth starts and stops, and how much it returned. **Largest Free Block** (diagnostic) shows what TLS needs.

The update check (`common/cloud-updates.yaml`) and the opt-in check-in now also wait for `ts_cloud::tls_ready()`. Before, they could start a second handshake next to one already running.

**The TLS heap gate (`common/tls_budget.h`):** every HTTPS request needs no other cloud request in flight, a **20 KB** largest free block and **48 KB** free in total.
- **Block:** mbedTLS's record IN buffer is the one big allocation: 16,717 B with IN 16384 / OUT 4096, CBC and SHA-384.
- **Total:** the handshake's estimated total is ~43.7 KB, including the request task's 8 KB stack.
- **Recipe fetch:** asks for 16 KB more (its 8 KB download buffer and the JSON).
- **History:** until 1.2.2-rc1 the gate asked for 48 KB in one block. On the bench with Bluetooth released (114,016 B free, largest block 31,744 B) it never passed, so no HTTPS ever started.

The Wi-Fi details are saved in flash and survive updates of the public build.

**Claiming the chamber in Home Assistant:** HA discovers `automyco-xxxxxx` and offers to add it. On adoption, HA generates a key unique to this chamber and sends it (`NoiseEncryptionSetKeyRequest`). The chamber saves it, and from then on it only accepts encrypted API connections with that key. Only a factory reset (serial erase) removes it.

**The claim window** (`provisioning: timeout: 30min`, substitution `provisioning_window`): from power-on until the chamber has joined Wi-Fi **and** has an API key. If the window expires first, BLE Improv shuts down and the API refuses new clients and key changes until the next power cycle. Power-cycling is the deliberate "I'm standing next to it" gesture. Owners who only use the iOS app or web UI never set an API key, so their window closes 30 minutes after every boot. That's intended: the web UI and app are unaffected, and the API stays shut to everyone. To add the chamber to HA later, power-cycle it and adopt it within 30 minutes.

**Firmware updates:**
- **Signed images only (1.2.3+, issue #32):** whoever uploads, and however (HA, `esphome upload`, the iOS app, fleet updates), the chamber only installs an image signed by the AutoMyco firmware key (ECDSA P-256). ESP-IDF checks it in `esp_ota_end()` before switching slots, so an unsigned, tampered or wrong-key image is refused and the running firmware carries on. Verify-on-update only: no hardware secure boot, no flash encryption, no eFuse written, bootloader not checked, so a serial install always recovers a chamber. The switches are in `terrashroom-public.yaml` (`esp32: framework: sdkconfig_options`), checked and completed by `components/fw_signing`, which embeds `signing/fw_pub.pem` (or, with a loud warning, the test key in `signing/test/`, which no one can sign for). `site/scripts/publish_firmware.py` signs releases with the key from SSM. If you build the public YAML yourself, your image is signed by nobody: switch once over serial, or build `terrashroom.yaml`, which doesn't verify. Keys, transition, recovery, bench test: docs/fleet-updates.md. The password, API key and pairing below decide *who may upload*; the signature decides *what may run*.
- **Encrypted OTA:** since ESPHome 2026.9, the API key also authenticates OTA uploads. Whoever holds the chamber's key can upload.
- **Plaintext OTA** (`esphome upload` with a password) is locked by a random password generated on every boot that nobody knows. ESPHome can't make OTA *require* encryption when the key is set at runtime (validation rejects `ota: encryption:` here), so this lock is what keeps a stranger on the LAN from flashing firmware that ignores the interlocks. To use password uploads, call the API action **`set_ota_password`** (HA: Developer tools → Actions → `esphome.terrashroom_xxxxxx_set_ota_password`) with a password of 12 to 64 characters. It's stored in flash, and `""` locks it again. The action only works over an encrypted connection, after the chamber has been claimed, and within 2 minutes of holding the rear button 2-4 s (the same password is the paired phone's control key, so setting it is a pairing; one per hold).
- **Update pairing (no Home Assistant needed):** this is how the iOS app gets a password. Hold the rear button for 2–4 s to open a 2-minute window (`Update Pairing Open` turns ON). The app then writes a random password it made to the write-only `Update Pairing` text, which stores it like `set_ota_password` and closes the window. One password per hold, 12 to 64 characters, and refused outside the window. The text never reports the password back. Instead the chamber publishes `Update Pairing Fingerprint` (64 bits of a SHA-256 of the stored password), and the app only reports success when it matches its own password. The fingerprint is cleared again after 60 s, so a hash of a hand-typed (weak) password doesn't sit on the API. So another LAN device racing it during the window, or a window that timed out, shows up as a failed pairing. **Known limit:** the password crosses the LAN once, in cleartext, when the app writes it. Anyone on the LAN can already drive the chamber over the same API, and OTA itself never sends the password. A short press (light) and the 5 s hold (self-test) are unchanged. Web pages can't use it, because web_guard blocks cross-site requests. Chambers on 1.0.0 don't have it: they need Home Assistant (`set_ota_password`) or a wired install to reach a version that does.
- **The Pair window.** The same 2–4 s hold is the one "Pair" gesture. For 2 minutes it opens update pairing (above) and lets `Camera Reset Login` through (normally only within 30 min of power-on), so the app can re-pair the camera without a power cycle: reset, the camera reboots, its own 30-minute claim window opens, the app claims it. The chamber still never learns the camera's login. The grow light (Top Light) blinks white twice as confirmation, then goes back exactly as it was (on/off, colour, brightness, effect; a running effect restarts). UV-C is never touched. On a chamber no phone has paired with yet, the window is also what allows **UV-C on** and a **Remote Pairing Code** (from anywhere, HA included). Home Assistant adoption: the hold doesn't reopen ESPHome's `provisioning:` window by itself (no supported way), but **Open Adoption Window** pressed inside it restarts the chamber with the window open for 30 minutes and asks an AutoMyco camera to open its own. Otherwise the window opens only after a real power-on: no longer after a software restart, an OTA update or a crash (`adopt_window.h`), because a keyless API accepts anyone's key.
- **Serial**, using the automyco.com flasher, always works and is the recovery path. For now it does a full erase-and-install, so it also resets the chamber's settings, Wi-Fi and API key.

**What's still exposed, and why it's acceptable or how to close it:**

| Risk | Window | Mitigation |
|---|---|---|
| Someone on the LAN claims an unclaimed chamber first, by setting their own API key (trust on first use) | First 30 min after a real power-on (or after Open Adoption Window), until the owner adopts it. Not after a restart: before, any LAN client could press Restart and get a fresh 30 minutes | Adopt soon after first boot. If HA reports the chamber as already encrypted with an unknown key, erase it over serial and start again. 2026.9 sends the key over a Noise session with the all-zeros PSK, which stops passive sniffing but doesn't authenticate either side. |
| Someone in Bluetooth range sends their own Wi-Fi details during the owner's 2-minute button authorisation | Only after a button press, and only inside the claim window | Physical presence is needed to open it at all. If the chamber turns up on the wrong network, power-cycle it and set it up again over the cable. |
| Safe mode (after 10 failed boots) runs only Wi-Fi and OTA, without the claim window | Safe mode only: it reboots back to normal after 5 min | `on_safe_mode` sets the OTA lock when safe-mode setup finishes. ESPHome's setup doesn't wait for Wi-Fi to connect (`WiFiComponent` has no `can_proceed` override), so the lock is in place before the chamber can be reached. With no access point configured, safe mode has no hotspot either. |
| Control is unauthenticated on the LAN (`web_server`) | Always | Same as the personal build (see [Network trust model](#network-trust-model)). The interlocks apply to every request, and UV-C is interlocked and time-limited. Use a trusted network or an IoT VLAN. |
| Someone on the LAN reads the chamber's climate history (`GET /history`, `components/chamber_history`): up to ~180 days of temperature, humidity, setpoints, heater/mist duty and phase | Always | Read-only, and the same readings `/events` already streams to anyone on the LAN. web_guard's Host and Origin checks keep web pages and DNS rebinding out, as for every request. It holds no identifiers, credentials or location. Details: `docs/device-api.md` "Climate history". |
| Someone on the LAN switches UV-C on, restarts the chamber, disables a water interlock, installs or changes update settings, or pairs remote access | Until a phone has paired, only UV-C on and the Remote Pairing Code are held back (rear-button Pair window); the rest stays open as before | The full keyed list is in `docs/device-api.md` "Security" (Restart, Camera Power Cycle, Remote Pairing Code, Water Lockout Override, Water Sensing, Atomizer Hard Stop clearing, Water Probe Drive, Manual Mist Limit, Install Firmware Update, Auto-Install Updates, Camera Install, besides these two). UV-C is also capped at 60 lit minutes in any 24 hours, and pressing it again never extends a run. Once a phone has paired (owner OTA password set), web_guard requires `X-AutoMyco-Key: <hex HMAC-SHA256(SHA-256("automyco-control:" + owner OTA password), "<METHOD> <request-target> <Control Challenge>")>` on every web request that could change `UV-C Lamp` (except `turn_off`) or press `Camera Reset Login`, in any URL spelling. `Control Challenge` is a single-use random value the chamber replaces after every accepted header, at boot and on a password change, so a captured header can't be replayed; no clock is involved. Until the password has loaded at boot those actions get 503 (`components/web_guard/control_key.h`, constant-time compare). Before that they stay open, so unpaired owners aren't locked out (logged). The chamber's own web page has no key, so it shows "Only the paired AutoMyco app can change this…" for the keyed controls; UV-C off, engaging the hard stop and switching the water override off always work. Home Assistant's native API is unaffected. Details: `docs/device-api.md` "Security". |
| A flash dump reveals the Wi-Fi password, API key and owner OTA password | Physical access | The same is true of any ESP32 without flash encryption. This project never burns eFuses. |
| Someone with the cable installs firmware that isn't signed | Physical access | Deliberate: the signature check is on update only, with no secure boot, so the owner can always recover or replace the firmware over serial. Over the network, only signed images install (1.2.3+). |

**Locking it down further:** owners who want no claim window, and OTA that *requires* encryption, can build `terrashroom.yaml` with their own `secrets.yaml`. That build's key and passwords are theirs alone, so its image must never be shared.

## Opt-in anonymous check-in

Both builds include `common/checkin.yaml`. It adds an **Anonymous Check-in** switch (on the web dashboard's
Chamber tab, under "Help the project"). The switch is **off by default**. While it's on, the chamber sends
`{"id": "<random 32 hex>", "v": "<version>", "b": "owner|public"}` to `https://automyco.com/api/checkin` once a
day, so the project can count how many TS-01s are still running. The ID is made on the chamber and isn't
derived from the MAC or the cloud device ID. The chamber keeps it across reboots, updates, power cuts and off/on
toggles, so each chamber is counted once. Turning the switch off stops the check-ins, and the **Reset Check-in ID**
button erases the ID (a new one is made at once if the switch is on). Firmware 1.2.0 and older erased the ID whenever
the switch was turned off, and also at any boot that restored the switch as off, which could count one chamber
twice. A check-in is only sent while the heater is off,
because the HTTP request blocks the main loop for up to the shared client's 10 s timeout. The server keeps no IP address, and a row
expires 120 days after its last check-in (`site/lambda/checkin-core.mjs`). Counts: `python3 site/scripts/stats.py`.
## Cloud grow recipes
`common/cloud-recipes.yaml` (public build) adds a **Cloud Recipe** select filled at runtime from `https://automyco.com/recipes/v1/index.json`, so new species presets ship as data instead of a firmware release. The chamber re-checks every value against the entity limits and `max_safe_temp_f` (`common/cloud_recipes.h`), caches the last good set in flash, and advances multi-stage recipes by Grow Day. The fetch runs in its own task on the shared `cloud_http` client from `common/cloud-identity.yaml`. Built-in Grow Presets still work offline, and recipes can't touch UV-C. Entities and rules: [`../docs/device-api.md`](docs/device-api.md#cloud-grow-recipes-commoncloud-recipesyaml). Publishing: `site/README.md`. Tests: `python3 -m unittest discover -s site/test -p '*_test.py'` (includes a host build of `cloud_recipes.h`, `test/cloud_recipes_host_test.cpp`).

## Grow phases
**Grow Phase** (Incubation / Pinning / Fruiting) changes what the controller aims for:
- **Incubation:** no misting, lights off, Incubation Temperature, sparse fresh air.
- **Pinning:** RH to the species' pinning RH (94 %, or 92 % for Lion's Mane and Black King) or the setpoint if higher, capped at 94 % (setpoint +3 % with no species known); fresh air twice as often.

When a phase has run its days, or a camera check says the block is ahead, the chamber asks before it moves on (Phase Check / Phase Question, answered with Advance Phase or Not Yet). Fruiting is the default, so nothing changes unless you pick another phase. Details: `../docs/grow-phases.md`. Home Assistant blueprints for the camera check and phone prompt: `../homeassistant/`.

## Remote access
`common/remote-access.yaml` (both main-board builds) lets the owner see the chamber on automyco.com/remote from anywhere. It is **off by default** and does nothing until the owner pairs the chamber with a code typed into **Remote Pairing Code**. Once paired:
- one sample a minute, POSTed every 5 min from its own task on the shared `cloud_http` client (TLS verified);
- **read-only**: the server can't send commands, and UV-C and every interlock stay local;
- if the cloud is off or gone, the chamber just backs off and keeps growing.

Design, costs and the kill switch: `../docs/remote-access.md`. Host test: `test/remote_access_host_test.cpp` (run by `site/test/remote_test.py`).

## Time without internet, and the optional DS3231 RTC
The light schedule, Grow Day, Phase Day and the phase check use the wall clock. Heater cut-offs, the UV-C auto-off and the low-water lockout never do (they run on `millis()`).

- **After one SNTP sync the clock keeps running** through internet outages, from the ESP32's crystal, and should survive a software restart. The **Time Source** diagnostic reads `sntp`, then `held` once there's been no sync for an hour. Everything keeps working.
- **A power-on with no internet has no clock** (Time Source `none`) until SNTP syncs. The climate control and every interlock run as usual. The light schedule, the day counters and the phase check wait.
- **`common/rtc-ds3231.yaml`** (opt-in, not in the public build) adds a DS3231 at 0x68 on the shared I2C bus (3V3 only). It sets the clock at boot (Time Source `rtc`) unless its oscillator-stop flag is set or the time is implausible, and it's rewritten after every SNTP sync:
  ```yaml
  packages:
    core: !include common/terrashroom-core.yaml
    rtc: !include common/rtc-ds3231.yaml
  ```

Details and the bench test: `docs/time-and-schedule.md` in the monorepo. Host tests: `tests/test_time_source.cpp`.

## Optional: non-contact water-level sensor
`common/water-level-sensor.yaml` (opt-in, not in the public build; issue #27) reads a capacitive level sensor (XKC-Y25-NPN class) stuck to the outside of the reservoir, on **GPIO19**. It adds **Level Sensor** to **Water Sensing** and a **Water Level Sensor** diagnostic:
```yaml
packages:
  core: !include common/terrashroom-core.yaml
  water_level: !include common/water-level-sensor.yaml
```
- **Fail-safe:** the pin is pulled toward "no water", so an unplugged or dead sensor reads dry. In Level Sensor mode Low Water starts ON and clears only after 6 s of water readings outside the atomizer's splash window, and comes back after 10 s dry. The Humidity Response check keeps running as a backup.
- **Polarity:** `water_level_active_low: "true"` (default) for an open-collector sensor that pulls LOW on water. Never feed the pin more than 3.3 V.
- **Commission before switching modes:** full = ON, empty = OFF, unplugged = OFF. Wiring diagram, mounting and parts: `docs/water-level-sensor.md` in the monorepo. Logic: `common/water_level.h`, host tests `tests/test_water_level.cpp`.

## Optional: SCD41 CO2 sensor
`common/scd41.yaml` adds a Sensirion SCD41 (CO2, plus its own temperature and humidity). The personal build (`terrashroom.yaml`) includes it. The public build doesn't, because stock chambers don't have one. To add it to another build, list it as a second package:
```yaml
packages:
  core: !include common/terrashroom-core.yaml
  scd41: !include common/scd41.yaml
```

**Entities:**
- **CO2:** ppm, in the Air section.
- **SCD41 Temperature and SCD41 Humidity:** diagnostics. The HDC1080 stays the chamber's main reading; these are a cross-check.
- **Calibrate CO2 (fresh air):** a button in the Maintenance section.

Without the sensor, the build logs one setup error at boot and the CO2 entities read unavailable. Nothing else changes.

**Wiring:**
- **Shared I2C bus:** it goes on the HDC1080's bus (3V3, GND, SDA on GPIO21, SCL on GPIO22) at address **0x62**, so it doesn't clash with the HDC1080 (0x40) or the INA219 (0x45).
- **Y-lead:** make a short one from the HDC1080 connector, using the pin order from the continuity check in `hardware/ts01-main-v2/MEASURE.md`.
- **Breakout board:** use one (Adafruit and SparkFun both make one with pull-ups) rather than a bare sensor.
- **Placement:** keep it out of the direct mist stream. It's rated to 95 % RH, non-condensing.

**Calibration:**
- **Self-calibration is off.** The sensor's automatic self-calibration assumes it sees ~400 ppm fresh air at least weekly. A closed chamber can sit far above that for days, so it would read low and call for too little fresh air. The factory calibration is used as is.
- **To correct drift:** put the sensor in fresh outdoor air, wait at least 3 minutes, and press **Calibrate CO2 (fresh air)**. It uses `scd41_fresh_air_ppm` (430 ppm, roughly the 2026 outdoor background); override it in the build's `substitutions:` if you know your local value.
- **After a restart:** check the fresh-air reading again. Whether the correction survives a power cycle hasn't been checked on this sensor.
- **Altitude:** set `scd41_altitude` to your height above sea level (default 0 m) for the pressure correction.

**CO2-driven fresh air** (issue #26, logic in `common/co2_control.h`, host tests in `tests/test_co2_control.cpp`): in Pinning and Fruiting, CO2 above **CO2 Limit** starts the core's fresh-air exchange and holds it open until CO2 is at or below the limit minus **CO2 Hysteresis** (200 ppm): at least Fresh Air Duration, at most **CO2 Exchange Max** (15 min), then a **CO2 Exchange Gap** (5 min) before the next CO2 start. It waits for a humidify cycle to finish, ends early if RH falls 10 % under the target, and ignores CO2 in Incubation. The timer schedule keeps running as the floor, and with the sensor missing or silent for 2 min the schedule alone runs. **CO2 Exchange** is ON while an exchange is held. Full description, arbitration with the humidity controller, wiring and parts: `docs/co2-fresh-air.md` in the monorepo.

**Recipe CO2 targets** (opt-in): with `common/scd41.yaml` and `common/cloud-recipes.yaml` in the build, adding `common/recipe-co2.yaml` makes the chosen Cloud Recipe set **CO2 Limit** in Pinning (`pinning_co2_ppm`) and Fruiting (`fruiting_co2_ppm`), on every stage apply and phase change. Incubation and recipes without a target leave it alone. The fields are only parsed in builds with this package (build flag `TS_RECIPES_CO2`), so the public image doesn't change. Host tests: `tests/test_recipe_co2.cpp`.

## Optional: Bluetooth proxy
`common/bluetooth-proxy.yaml` turns the chamber into a Home Assistant Bluetooth proxy: it forwards nearby BLE advertisements (plant sensors, thermometers) to HA and lends HA up to 2 active connections. No build includes it by default. To add it, uncomment or add the package line in your build (it needs `fleet_health`, which both main-board builds already include):
```yaml
packages:
  core: !include common/terrashroom-core.yaml
  # ...the build's other packages...
  # bluetooth_proxy: !include common/bluetooth-proxy.yaml   # HA Bluetooth proxy + heap diagnostics
```
A public build you compile yourself with the proxy is still a public build to automyco.com. If **Check for Updates** or **Auto-Install Updates** installs a release, the release replaces your image and the proxy is gone. Turn Auto-Install off, or add the proxy to the personal build instead.

**Compatibility (checked against ESPHome 2026.9.1):**
- **Works on the core's `framework: arduino`.** ESPHome builds Arduino as an ESP-IDF component, so `bluetooth_proxy` has no framework restriction (`esphome/components/bluetooth_proxy/__init__.py`). Switching to ESP-IDF isn't needed.
- **Coexists with `esp32_improv`** (the public build's BLE Wi-Fi setup). Both use the one `esp32_ble` stack. `esp32_ble: max_connections: 3` covers the proxy's 2 slots plus Improv's GATT server. Since 1.2.2 the public build shuts Bluetooth down after Wi-Fi setup to free heap for TLS (see [Public build](#public-build)). With the proxy in the build it can't, so the stack stays on from boot (`ble_release.h` checks for `esp32_ble_tracker`). The proxy then pays the stack's whole cost, the RAM that 1.2.2 frees. Watch **Heap Low**.
- **Settings:** `ble_proxy_slots: "2"` (ESPHome's default is 3), `ble_proxy_active: "false"` for advertisements only, and the default scan duty cycle (30 ms every 320 ms), which leaves most of the shared 2.4 GHz radio to Wi-Fi. The grow loop and interlocks don't use the network, so a busy radio only slows the web UI and HA.

**Cost, measured by compiling `terrashroom-public.yaml` with and without the package (2026-10-09, fleet health and cloud updates included):**

| Build | App image | Static DRAM (of 124,580 B) |
|---|---|---|
| Public, Arduino (shipped) | 1,559,839 B (85.0 % of the 1,792 KB OTA slot) | 81,684 B |
| Public + proxy, Arduino | 1,643,331 B (89.6 %), **+83,492 B** | 85,124 B, **+3,440 B** |
| Public, ESP-IDF (not shipped, see below) | 1,458,023 B (79.5 %) | 78,964 B |
| Public + proxy, ESP-IDF | 1,540,587 B (84.0 %), +82,564 B | 82,380 B, +3,416 B |

The personal build plus the proxy also compiles: 1,602,747 B, 83,732 B static DRAM. The proxy fits on Arduino with ~187 KB of the app slot left. That headroom is the main cost: later features have less room.

**Runtime heap (estimate; not yet measured on a chamber).** Static DRAM says little about this. Bluedroid, Wi-Fi, lwIP and TLS all allocate at runtime. Budget for the public build:
- **Idle without the proxy** (Wi-Fi, API, web server, Bluedroid for Improv): roughly 90–120 KB free.
- **Proxy scanning:** about 10–20 KB more (GATT client, scan-result queue, advertisement batches to HA).
- **Each active connection:** about 2–4 KB, plus whatever HA reads through it.
- **Each HA API or web UI client:** about 5–10 KB.
- **TLS** (update checks, Share Diagnostics reports): about 44 KB, including one **16.7 KB contiguous** record buffer. `cloud_post.h` starts a request only with a 20 KB block and 48 KB free (`common/tls_budget.h`).

That leaves about 60–100 KB free with the proxy, so a TLS request should still succeed. Fragmentation matters as well as the total: no request starts while the largest free block is under 20 KB, even with plenty of heap free overall. The package adds **Heap Largest Block** (ESPHome `debug`), **Loop Time** and a **Heap Low** problem sensor. Heap Low turns on when the TLS gate would refuse a request (block under 20 KB or free under 48 KB) or **Lowest Free Heap** is under 20 KB. Free Heap and Lowest Free Heap come from `fleet-health.yaml`. If Heap Low stays on, drop to `ble_proxy_slots: "1"`, then to `ble_proxy_active: "false"`. Put real numbers here after the first bench session with the proxy on.

**What switching the core to ESP-IDF would take** (compile-tested 2026-10-09, not shipped):
- It saves ~100 KB of flash and ~2.7 KB of static RAM. Usually IDF also has more free heap at runtime, but that hasn't been measured on this board.
- Two things break:
  - The water-probe lambda in `common/terrashroom-core.yaml` uses Arduino's `pinMode`/`digitalRead`. The fix is `gpio_set_direction`/`gpio_pulldown_en`/`gpio_get_level`, which works on both frameworks.
  - `fleet_health.h` uses Arduino-ESP32's `set_arduino_panic_handler` for the crash backtrace. IDF would need another hook, for example wrapping `esp_panic_handler` or using a core dump.
- With both patched, the public build compiled on IDF with and without the proxy.
- Switching changes the bootloader and sdkconfig and puts every chamber on a new framework, so do it as its own change with a bench session, not as part of adding the proxy.

## Optional: lid reed switch (UV-C interlock)
`common/lid-interlock.yaml` (opt-in, not in the public build; issue #28) reads a normally-open reed contact on the lid, between **GPIO27** and GND with the internal pull-up, and adds **Lid Closed**:
```yaml
packages:
  core: !include common/terrashroom-core.yaml
  lid: !include common/lid-interlock.yaml
```
- **UV-C Lamp is refused** unless the lid has read shut for 500 ms (`UV-C blocked: lid open`), and a lit lamp is **cut on the first open reading** (`UV-C cut: lid opened`), by the reed GPIO's own edge trigger and by a 50 ms watchdog that reads the pin directly. Nothing in it ever turns UV-C on.
- **Fail-safe:** a broken lead, a missing switch or a lost magnet reads as lid open. With the package in and no switch fitted, UV-C can never turn on, which is why it isn't in the public build.
- **Checked by CI:** `tools/check_firmware_safety.py` rule 5 fails a build that has a lid sensor but lets UV-C turn on without checking `uv_lid_ok`, or whose GPIO lid sensor doesn't cut UV-C itself or has time filters.
- **Hardware interlock recommended too:** a second reed in series with the UV-C drive. Wiring, GPIO choice, commissioning (with UVC1 unplugged) and parts: `docs/lid-interlock.md` in the monorepo. Logic: `common/lid_interlock.h`, host tests `tests/test_lid_interlock.cpp`.

## First flash (over serial, with the owner at the bench)
Prerequisites: a verified stock backup (`dumps/SHA256SUMS`), the eFuse gate passed, and the wiring from `../PINMAP.md` with adapter RTS on header pin 3.

1. Stop anything holding the serial port: `pkill -f uart_log.py`
2. Unplug **UVC1** for the first boot.
3. **Hold the rear Bluetooth button** (it pulls IO0 low) and run the command below. Let go once it says `Connected`. `esptool.cfg` handles the reset over RTS.
   ```sh
   esptool --chip esp32 -p /dev/cu.usbserial-AB0KSXC6 -b 230400 \
     write-flash --erase-all 0x0 esphome/.esphome/build/terrashroom/build/firmware.factory.bin
   ```
   `--erase-all` wipes the old stock settings (NVS still holds the Wi-Fi password and Blynk config) in the same session, so the button only has to be held once.
4. Watch the first boot: `uv run --with pyserial python tools/listen.py 60 --reset`. Check that:
   - the I2C scan finds the HDC1080 at 0x40, plus any other devices (the stock "electricity" current sensor at 0x45, and the SCD41 at 0x62 if fitted)
   - the temperature and humidity readings look sane
   - the Low Water state is correct with the reservoir full and empty
5. After that, update over Wi-Fi with `esphome run terrashroom.yaml`.

**To go back to stock:** hold the button and run `esptool ... write-flash 0x0 dumps/terrashroom_stock.bin`.

## Bench checklist: what still needs confirming
| Item | Why |
|---|---|
| Fan 1/Fan 2/Blower/Atomizer/Heater each move the right part | Pin roles come from static analysis. Run each at low power for a few seconds and watch. |
| Minimum fan duty | Stock idles the fans at about 3% (8/255). Low duties may stall some fans. |
| Atomizer drive | Stock drives it with 10 kHz PWM at full duty. Confirm the mist output. |
| ~~Grow light (Top Light)~~ | **Confirmed**: 24 LEDs on GPIO5 are the ring lamp at the top of the arch (seen white on camera). |
| **Bottom Light (base rim, GPIO14) stays dark** | Worked on stock. Under ESPHome only one LED shows a stuck green, even with stock's exact bit timing (400/850/800/450 ns, 300 µs reset), 40 mA drive and the strip driven directly. Data reaches the STRIP connector (~0.25 V average while streaming). Still open. |
| Preset values | These are community starting points. Stock preset values are documented in `analysis/firmware_findings.md`. |
| ~~The camera board~~ | **Done:** the chamber installs the AutoMyco camera firmware over the stock camera's updater (`docs/camera-install.md`). The camera link has run on the owner's unit (2026-10-09). The public camera build and the app-driven install haven't run on a stock camera yet. |
| **Public build on a real chamber** | 1.2.0 ran on the owner's chamber (2026-10-11): free heap 29,952 B, lowest 120 B, so no TLS (fixed in 1.2.2, see [Public build](#public-build)). Check on 1.2.2: Free Heap, Lowest Free Heap and Largest Free Block after 30+ min; the flasher's Wi-Fi step on an erased chamber (state, network list, join, reported URL; the RTS line must stay released so EN isn't held low); BLE Improv with the rear button; HA adoption sets the key; the claim window closes after 30 min; `set_ota_password` then `esphome upload` works. |

