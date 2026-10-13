# TS-01 local device API (contract between firmware and apps)

The replacement firmware is ESPHome. It exposes everything over the **ESPHome web_server (version 3) REST + Server-Sent Events API** on port 80. It also exposes the ESPHome native API for Home Assistant. Apps (iOS first) talk to the chamber **directly on the LAN** and need no cloud.

## Discovery
- mDNS hostname: `automyco-<last 6 hex digits of MAC>.local`. For example, `automyco-a1b2c3.local`. Builds from before 2026-10-09 used `terrashroom-<hex>`, so apps should accept both prefixes.
- Bonjour services advertised by ESPHome: `_http._tcp` (port 80) and `_esphomelib._tcp` (port 6053). Apps should browse `_esphomelib._tcp` and resolve to host:80. If browsing finds nothing, the user enters the IP or hostname manually.
- `GET /` serves the chamber's own web dashboard (`esphome/web/dashboard.js`, embedded and served as `/0.js`). It uses only this API, so it is also a working example of it.

## Live state: `GET /events` (SSE)
On connect, the device sends one `state` event per entity, then sends another each time a state changes. The event data is JSON. Examples:
```
event: state
data: {"id":"sensor-temperature","name":"Temperature","state":"72.4 °F","value":72.4}
event: state
data: {"id":"switch-atomizer","name":"Atomizer","state":"OFF","value":false}
event: state
data: {"id":"fan-fan_1","name":"Fan 1","state":"ON","speed_level":40}
event: state
data: {"id":"select-grow_preset","name":"Grow Preset","state":"Blue Oyster","option":["Custom", "..."]}
```
**Verified on hardware with ESPHome 2026.9:** `id` is name-based, `<domain>/<Entity Name>` (e.g. `"switch/Climate Control"`). Older ESPHome versions used `<domain>-<object_id>`. Apps must accept both forms and tolerate extra fields, missing fields, and `ping` / `log` events.

## Commands: `POST /<domain>/<Entity Name>/<action>?<params>`
Use the percent-encoded entity **name** as the path segment (`/switch/Climate%20Control/turn_on`). The legacy object_id paths return 404 on ESPHome 2026.9. POSTs **must** send `Content-Length: 0` (an empty body), or the device answers `411`. From firmware 1.0.1, POSTs **must** also send `X-Requested-With: <any value>` (apps) or come from the chamber's own page (same-origin `Origin`); actions sent as GET answer `405`. See [Security](#security).
| Domain | Actions / params |
|---|---|
| switch | `turn_on`, `turn_off`, `toggle` |
| fan | `turn_on?speed_level=0-100`, `turn_off` |
| light | `turn_on?brightness=0-255&r=&g=&b=`, `turn_off` |
| number | `set?value=<n>` |
| select | `set?option=<url-encoded option>` |
| button | `press` |

A `GET /<domain>/<Entity Name>` returns that entity's current state as JSON.

## Climate history: `GET /history?since=<epoch>`
Firmware with `esphome/components/chamber_history` (both builds, `common/terrashroom-core.yaml`) keeps its own climate history in flash, so apps can draw weeks of charts over the LAN with no cloud and no app left open. Older firmware answers `404`: apps skip it silently.

**Request.** `GET /history` returns everything held; `?since=<UTC epoch seconds>` returns only records strictly newer than that (apps pass the newest record time they already have). Read-only like every other GET: no `X-Requested-With`, no key; web_guard's Host and Origin checks apply as for any request, and a build with `web_server` `auth:` would apply it here too. The chambers' LAN API has no auth by design (see [Security](#security)); the history is the same climate data `/events` already streams.

**Response.** `200`, `Content-Type: text/csv`, chunked, oldest first (ring order; across a backwards clock step the order can break, so sort by `time`):
```
# automyco-history 1 layout=stock interval=300 capacity_days=179.9 now=1791500000
time,temperature_f,humidity_pct,temperature_target_f,humidity_target_pct,heater_pct,mist_pct,co2_ppm,phase
1791499500,72.4,88.1,70,88,35,10,,3
1791499800,72.5,87.9,70,88,30,15,,3
```
- `#` lines are comments (format version `1`, `layout` = `stock` / `esphome`, `now` = the chamber's clock, 0 if unset). The next line names the columns: find them by name, firmware may append columns.
- `time`: UTC epoch seconds of the **start** of the record's 5 minutes, to the minute. Records close on the wall-clock 5-minute grid (:00, :05, ...), so after boot the first one can be shorter.
- `temperature_f`, `humidity_pct`: means over the 5 minutes of every reading the sensors published (0.1 resolution). `temperature_target_f`, `humidity_target_pct`: the setpoints at the end (whole numbers). `heater_pct`: heater duty (on-time × PWM level) over the 5 minutes; `mist_pct`: atomizer on-time; both 0-100 in 5 % steps. `co2_ppm`: mean CO2 (10 ppm steps) when the SCD41 package is fitted. `phase`: 0 = Climate Control off, 1 Incubation, 2 Pinning, 3 Fruiting.
- An empty field is unknown (no reading in that interval, sensor fault, no CO2 sensor).
- `503` `history unavailable: …` (text/plain): the chamber has no known history partition (see below) or couldn't read it.

**Storage.** A ring of 4 KiB sectors in a raw data partition nothing else uses, found at boot by label, subtype, address and size. Partition tables can't change over OTA, so there are two layouts, and anything else leaves history off:

| Chambers | Partition | Size | Holds (at 1 record / 5 min) |
|---|---|---|---|
| installed over the stock firmware's own Bluetooth OTA (`tools/ble_ota.py`), which keeps the stock partition table | stock `storage`, SPIFFS subtype, `0x370000` | 512 KiB, 128 sectors | **~180 days** (127 full sectors × 408 records) |
| flashed over USB (web installer, ESPHome's 4 MB table) | ESPHome `spiffs` (data/spiffs, after the app slots) | 60 KiB, 15 sectors | **~20 days** (14 × 408; ESPHome's 384 KiB NVS stays untouched: it holds Wi-Fi and every saved setting) |

Never touched: `nvs`, `otadata`, `phy_init`, the app slots, `eeprom`, and the stock `coredump` (`0x3F0000`). The stock firmware's old SPIFFS contents are dead on migrated chambers (the stock app is gone); each sector is erased only when the ring first reaches it.

Format (`esphome/components/chamber_history/history_log.h`, host tests `esphome/tests/test_chamber_history.cpp`): each sector is a 16-byte header (`AMYH` magic, sequence number, base time rounded to the minute, version, record size, CRC-16) and 408 records of 10 bytes (u16 minutes after the base, 56 bits of packed readings, CRC-8 seeded with the sector's sequence). The highest valid sequence is the sector being written. Power loss: a sector is erased before its header is written and the header before any record, so a cut leaves either a sector with no valid header (ignored, erased again when the ring reaches it) or one torn record (CRC fails, the slot is skipped and never rewritten). One sector erase per ~34 hours: decades of flash life even on the 15-sector layout.

**Without a clock.** Records need wall-clock time (SNTP, the held clock after a software restart, or the optional DS3231). Until the clock is set, intervals run 5 minutes by uptime and wait in RAM (up to 6 hours) to be written with their real times; a power cut before the clock arrives loses them.

**Clients.** iOS `ChamberHistorySync` (`apple/AutoMyco/ShroomChamber/Model/ChamberHistory.swift`) fetches on connect and merges where the app recorded nothing, ahead of cloud history. Mock: `apple/AutoMyco/mock_server/esphome_mock.py` serves the same CSV.

**Check on hardware** (read-only): `curl -s "http://$H/history" | head -5` shows the comment, the header and the oldest rows; `curl -s "http://$H/history?since=$(($(date +%s)-900))"` the last ~15 minutes.

## Entities (object_id is the slug of the name. Apps key on it internally, but the wire uses the Name)
| Domain | object_id | Name | Notes |
|---|---|---|---|
| sensor | `temperature` | Temperature | °F, from the HDC1080 |
| sensor | `humidity` | Humidity | %RH, from the HDC1080 |
| sensor | `wifi_signal` | WiFi Signal | dBm |
| sensor | `supply_voltage` | Supply Voltage | V, 24 V load rail (INA219) |
| sensor | `load_current` | Load Current | A, current into the switched loads |
| sensor | `load_power` | Load Power | W |
| sensor | `co2` | CO2 | ppm. Only in builds with `common/scd41.yaml` (the personal build); unavailable if no SCD41 is fitted |
| sensor | `scd41_temperature` | SCD41 Temperature | °F, diagnostic (`common/scd41.yaml`) |
| sensor | `scd41_humidity` | SCD41 Humidity | %RH, diagnostic (`common/scd41.yaml`) |
| button | `calibrate_co2__fresh_air_` | Calibrate CO2 (fresh air) | forced recalibration to `scd41_fresh_air_ppm` (430 ppm default). Sensor in fresh air for ≥ 3 min first (`common/scd41.yaml`) |
| binary_sensor | `low_water` | Low Water | ON means the reservoir needs refilling. Follows Water Sensing: the debounced probe in `Probe` mode, Tank Empty Alert in `Humidity Response` mode, the opt-in level sensor or Tank Empty Alert in `Level Sensor` mode (fails closed: ON until the sensor reports water for 6 s). grow_state `low_water` also covers an engaged Atomizer Hard Stop. From 1.2.0 the probe fails closed: ON at boot until three wet readings in a row (6 s), cleared only by three wet readings, and not read while the atomizer runs or for 3 s after (splashes). A Tank Empty Alert survives a reboot together with the Hard Stop it engaged |
| binary_sensor | `water_probe` | Water Probe | Diagnostic. ON = the conductivity probe sees water (debounced), whatever Water Sensing is set to |
| binary_sensor | `water_level_sensor` | Water Level Sensor | Diagnostic, builds with `common/water-level-sensor.yaml` only. ON = the non-contact level sensor sees water (debounced: 6 s to clear, 10 s to go dry), whatever Water Sensing is set to. A missing or unplugged sensor reads OFF (`docs/water-level-sensor.md`) |
| text_sensor | `grow_state` | Grow State | `idle`, `incubating` (Incubation phase: no misting), `humidifying`, `topping_up` (blower only near target), `drying`, `purging` (RH > 98 %), `heating`, `fresh_air`, `low_water`, `sensor_drying`, `sensor_error`, `paused` |
| text_sensor | `mist_status` | Mist Status | Why the mister is or isn't running, e.g. `misting`, `cycle: resting`, `at target`, `fresh-air exchange`, `over-humidity purge (>98 %)`, `low water`, `hard stop`, `climate control off` |
| select | `grow_preset` | Grow Preset | `Custom` plus 13 species presets from the stock firmware |
| select | `cloud_recipe` | Cloud Recipe | Builds with `common/cloud-recipes.yaml` (the public build). `None` plus the recipes cached from beezy.ninja. **The options change at runtime** when a new recipe set arrives: re-read the option list from the SSE `state` event / `GET /select/Cloud%20Recipe` after reconnecting (Home Assistant sees new options on its next reconnect). Choosing one sets Grow Preset to `Custom` and applies the recipe's stage for the current Grow Day. A hand edit of the temperature or humidity setpoint, or choosing a built-in Grow Preset, sets it back to `None`; the setpoints stay as they are |
| switch | `remote_access` | Remote Access | Builds with `common/remote-access.yaml` (both main-board builds). Config, default OFF, restored. On + paired: the chamber POSTs its state to beezy.ninja every 5 min (read-only remote access, `docs/remote-access.md`). Off: nothing is sent |
| text | `remote_pairing_code` | Remote Pairing Code | Config. Write the 8-character code from beezy.ninja/remote to pair (turns Remote Access on). Always reads back empty |
| text_sensor | `remote_frames` | Remote Frames | **Camera board** (public build, `common/camera-remote.yaml`): `on` while the chamber has handed it frame-upload credentials (Remote Access on and paired), `live (someone is watching)` while uploads run every 30 s, else `off`. The owner build (`camera.yaml`) has it too, beside its frame-ingest uploads. With `Frames Uploaded` / `Frame Upload Failures` (diagnostic counters) |
| text_sensor | `remote_status` | Remote Status | Diagnostic: `Off`, `Pairing...`, `Paired`, `Connected`, `Not paired: ...`, `Paired; remote access is off for this account or chamber`, `Can't reach beezy.ninja; retrying`, ... |
| text_sensor | `cloud_recipe_stage` | Cloud Recipe Stage | `none`, or e.g. `2/3 (from day 15)`: the stage of the chosen cloud recipe now applied. Stages advance with Grow Day |
| text_sensor | `cloud_recipes_status` | Cloud Recipes Status | Diagnostic: e.g. `rev 3, 13 recipes 10-09 08:02`, `fetch failed; using cached rev 3`, `rejected ...: <why>` |
| button | `refresh_cloud_recipes` | Refresh Cloud Recipes | Diagnostic: fetch the recipe index now (it is also fetched 2 min after boot and every 24 h) |
| number | `humidity_setpoint` | Humidity Setpoint | %RH, 50–99 |
| number | `temperature_setpoint` | Temperature Setpoint | °F, 55–85 |
| number | `fresh_air_interval` | Fresh Air Interval | minutes between fresh-air exchanges |
| number | `fresh_air_duration` | Fresh Air Duration | seconds per exchange |
| number | `light_hours` | Light Hours | hours of light per day, 0–16 |
| switch | `climate_control` | Climate Control | master auto mode. When ON, firmware drives atomizer, fans and heater toward the setpoints. Off on a fresh chamber; after that it comes back after a power cut as it was, unless the power was off more than 72 h (see Grow Paused). Turning it on clears Grow Paused. |
| binary_sensor | `grow_paused` | Grow Paused | ON when the chamber came back from a power cut of more than 72 h with Climate Control on: it turned Climate Control off rather than start heating and misting by itself. The grow stays recorded. Apps should offer **Resume** (turn Climate Control on) or **End Grow**. Judged once per boot, only after a real power cut (power-on or brownout reset), only if the clock syncs within 10 min of boot and the chamber knew the time before the cut; otherwise it resumes. A restart (OTA update, Restart, crash, watchdog) never pauses |
| switch | `atomizer` | Atomizer | manual control (overrides auto until Climate Control is turned back on). A turn-on with Low Water (no bench override) or the Hard Stop is refused at once (`Atomizer blocked: low water` / `hard stop` in the Event Log) and does not pause Climate Control. A manual turn-on switches itself off after Manual Mist Limit |
| switch | `heater` | Heater | manual control |
| fan | `fan_1` | Fan 1 | speed 0–100. **Exhaust** (blows out; bench-confirmed) |
| fan | `fan_2` | Fan 2 | speed 0–100. **Intake** (inferred: Fan 1 confirmed as exhaust) |
| fan | `blower` | Blower | speed 0–100 |
| light | `top_light` | Top Light | **Grow light**: 24-LED ring lamp at the top of the arch (GPIO5). RGB + brightness + effects. In Climate Control, the Light Hours schedule switches it on at Light Start Hour (default 08:00) and off after Light Hours, keeping the last colour; a manual change sticks until the schedule's next switch. Colour, brightness and effect survive reboots. Send `effect=None` with `r/g/b` to stop a running effect, which otherwise hides the colour. |
| light | `bottom_light` | Bottom Light | 64-LED base rim strip (GPIO14). Not lighting yet; see esphome/README. |
| button | `restart` | Restart | |
| button | `run_self-test` | Run Self-Test | Runs each load alone for 5 s (no UV) and judges it by its current jump, bracketed by a baseline before and after; sets the `*_not_detected` sensors. The rear button held 5 s does the same. Climate Control keeps its setting: the controller sits out for the ~80 s the test takes, then carries on. While it runs, light effects pause (restored after), the light schedule and the auto sensor dry pulse sit out, and camera power cycles are deferred. The interlocks still apply. |
| switch | `boot_self-test` | Boot Self-Test | Config, default ON, saved on the device. Runs the self-test once after every boot, once Load Current and Temperature have readings, the chamber has been up 90 s, and the camera has either said ready or is off (at 180 s it goes ahead regardless; a camera still booting draws a changing current). So about 90–180 s after power-up. Off = only the passive check and the button |
| binary_sensor | `fan_1_not_detected`, `fan_2_not_detected`, `blower_not_detected`, `atomizer_not_detected`, `heater_not_detected` | Fan 1 / Fan 2 / Blower / Atomizer / Heater Not Detected | Diagnostic, `problem` class. ON = the load was switched on but the 24 V rail didn't see its current: unplugged or broken. Set by the self-test (at once) and by the passive check (two misses in a row); one detection clears it. OFF until a check says otherwise, and not kept across reboots. See "Load checks" below |
| text_sensor | `firmware_version` | Firmware Version | `esphome.project.version` of the running build (e.g. `0.1.0`), published at boot. Apps compare it with releases on beezy.ninja. Builds before this entity existed only advertise it in mDNS (see below) |
| text_sensor | `time_source` | Time Source | Diagnostic: where the wall clock comes from. `sntp` (synced within the last hour), `held` (valid clock but no sync for an hour, or none since a software restart that kept the clock: everything keeps working), `rtc` (set from the optional DS3231 at boot, no sync yet), `none` (no clock: light schedule, Grow Day, Phase Day and the phase check wait). Updates every minute. Builds up to 1.2.0 don't have it. See `docs/time-and-schedule.md` |
| text_sensor | `self-test_result` | Self-Test Result | One item per load, `<Load> <VERDICT> <detail>; `, e.g. `Fan1 OK 41mA; Fan2 OK 39mA; Blower UNSURE (noisy rail) -2mA spread 4mA; Atomizer SKIPPED (low water or hard stop); Heater OK 141mA;`. Verdicts: `OK`, `FAIL` (quiet rail, delta below the threshold: sets `*_not_detected`), `UNSURE` (inconclusive: never sets or clears `*_not_detected`) and `SKIPPED`. UNSURE reasons: `(noisy rail)` the readings or the two baselines spread wider than the load's limit, with `spread <n>mA`; `(rail changed)` UV-C, a light or Camera Power changed, or a light effect/transition ran, during the measurement; `(listen for it)` the blower (a 5 V load, ~6 mA on the 24 V rail, about the sensor's noise) read below its threshold on a quiet rail; it is never called FAIL. `Heater SKIPPED (no temperature reading)` when the heater refuses to start. Apps should treat `UNSURE` as "couldn't tell", not as a failure |
| switch | `water_lockout_override` | Water Lockout Override | Bench only: lets the atomizer run while the water probe misreads. Off at boot, auto-off after 10 min. |
| select | `water_probe_drive` | Water Probe Drive | Bench only: `Pulse (stock)` / `Held high` / `Held low` on GPIO15, so a meter can measure the probe. Back to stock at boot and 5 min after the last change |
| select | `water_probe_pull` | Water Probe Pull | Bench only: `Pull-down (stock)` / `None` / `Pull-up` on GPIO23. Same 5-min revert. While either bench setting is active, Probe-mode sensing fails closed: Low Water reads ON (atomizer locked out, UV-C cut) |
| binary_sensor | `water_probe_raw` | Water Probe Raw | Diagnostic. GPIO23 as read, no debounce: every 2 s, or every 0.5 s while the drive is held |
| text | `grow_name` | Grow Name | max 40 chars. Set with `POST /text/Grow%20Name/set?value=<urlencoded>` |
| button | `start_grow` | Start Grow | starts the grow session (day counter) and turns Climate Control ON |
| button | `end_grow` | End Grow | ends the grow session and idles the chamber: Grow Day goes to `NA`, Climate Control turns off, and the atomizer, heater, fans and blower switch off (lights and UV-C untouched). Does nothing when no grow is running. Apps should confirm first |
| sensor | `last_power_outage` | Last Power Outage | diagnostic: best estimate of the minutes the power was off before this boot, within about 8 min either way (the chamber saves the time every 15 min, so a cut can come up to ~16 min after the last save; this is the middle of that window, and the 72 h Grow Paused rule uses the same number). Set only after a real power cut (power-on or brownout reset). No state after a restart (OTA update, Restart, crash, watchdog; the power never went off), or when unknown (first boot, no clock last run, or no clock within 10 min of boot). The event log says `power was off 2 h 53 m to 3 h 10 m: resumed …` (`up to 0 h 11 m` for a short one) after a power cut, and `restarted (software): resumed …` (or `crash`, `watchdog`, …) after a restart. (Pre-release 1.2.0 builds logged restarts as `power was off` too, with the upper bound.) |
| sensor | `grow_day` | Grow Day | day number of the current grow; `NA` when no grow is running |
| select | `grow_phase` | Grow Phase | `Incubation` / `Pinning` / `Fruiting` (default). Changing it restarts the phase clock. Incubation: no misting, lights off, Incubation Temperature, sparse fresh air. Pinning: RH +3 % (cap 94), fresh air ×2. See `docs/grow-phases.md` |
| sensor | `phase_day` | Phase Day | 1-based day in the current phase; `NA` when no grow is running |
| select | `phase_advance` | Phase Advance | `Ask me` (default: raise Phase Check) / `Automatic` / `Manual` |
| binary_sensor | `phase_check` | Phase Check | ON = the chamber is asking whether to move to the next phase. Apps should show Phase Question with Advance Phase / Not Yet |
| text_sensor | `phase_question` | Phase Question | the question, e.g. `Pinning day 6: do you see pins (tiny mushroom bumps)? Start fruiting?`; empty when none |
| button | `advance_phase` | Advance Phase | yes: move to the proposed phase (or the next one when no question is open) |
| button | `not_yet` | Not Yet | no: snooze the question for Phase Snooze days and discard the camera observation |
| number | `incubation_days` | Incubation Days | d, 1–60, default 14 |
| number | `pinning_days` | Pinning Days | d, 1–21, default 5 |
| number | `incubation_temperature` | Incubation Temperature | °F, 60–82, default 75 (heater target during Incubation) |
| number | `phase_snooze` | Phase Snooze | d, 1–7, default 2 |
| select | `camera_stage` | Camera Stage | `Unknown` / `Colonizing` / `Colonized` / `Pinning` / `Fruiting`. Written by a vision check (Home Assistant blueprint), never by the firmware. Counts for 36 h; can open Phase Check early |
| text_sensor | `event_log` | Event Log | newest-first lines `MM-DD HH:MM text` (lines from before SNTP sync use `+<n>s`), ~600 B ring, persistent |
| binary_sensor | `tank_empty_alert` | Tank Empty Alert | ON when humidity-response sensing decides the tank is dry (atomizer 5 min on, RH +<1.5 %) |
| switch | `atomizer_hard_stop` | Atomizer Hard Stop | blocks every atomizer path and survives reboots. Turn OFF after refilling to clear |
| switch | `anonymous_check-in` | Anonymous Check-in | Opt-in, OFF by default, survives reboots. While on, the chamber POSTs `{id, v, b}` (a random install ID, the firmware version and the build) to `https://automyco.com/api/checkin` once a day (`beezy.ninja` on 1.1.0 and older). The ID is kept across reboots, updates, power cuts and off/on, so a chamber is counted once; turning the switch off stops the check-ins. (Up to 1.2.0, turning it off erased the ID.) Details: `esphome/common/checkin.yaml`. Apps may offer it, but must never turn it on without the owner choosing to. |
| button | `reset_check-in_id` | Reset Check-in ID | Config, newer than 1.2.0. Erases the anonymous check-in ID. If Anonymous Check-in is on, a new random ID (unrelated to the old one) is made at once; if it is off, no ID is kept until the switch is turned on again. Apps may offer it next to the switch. |
| binary_sensor | `update_pairing_open` | Update Pairing Open | Public build only. ON for 2 min after the rear button is held for 2–4 s (the **Pair window**), until a password is accepted. Apps use it as "the Pair window is open". The same hold blinks Top Light twice (then restores it exactly) and lets **Camera Reset Login** through for 2 min from the hold, even after the password was accepted. |
| text | `update_pairing` | Update Pairing | Public build only, write-only (mode password, state always empty). While Update Pairing Open is ON, writing 12–64 characters sets the OTA password and closes the window. Apps generate a random password, write it, and treat it as paired only when Update Pairing Fingerprint matches theirs. A different fingerprint means another device paired first. See `esphome/README.md` ("Update pairing"). |
| text_sensor | `update_pairing_fingerprint` | Update Pairing Fingerprint | Public build only. Cleared when a pairing window opens and 60 s after it's set. After a password is accepted, hex of the first 8 bytes of SHA-256(`automyco-ota-pair:` + password) (`esphome/update_pairing.h`). |
| text_sensor | `control_challenge` | Control Challenge | Public build only. 32 lowercase hex (16 random bytes): the single-use challenge apps sign `X-AutoMyco-Key` over (see [Security](#security)). Replaced the moment a signed request is accepted, at every boot, whenever the owner password changes, and on a paired chamber at least every 5 minutes (so a signed request that was held back expires); published over SSE and `GET /text_sensor/Control%20Challenge`. Not a secret. |
| select | `water_sensing` | Water Sensing | `Probe` / `Humidity Response` (lockout ignores the probe; dry-tank detection by RH response) / `Level Sensor` (only in builds with `common/water-level-sensor.yaml`: the non-contact sensor, with the RH-response check as a backup). Apps should take the options the device reports |
| select | `fresh_air_mode` | Fresh Air Mode | `Both fans` / `Alternate` / `Two-phase` |
| select | `intake_fan` | Intake Fan | `Fan 1` / `Fan 2`, default `Fan 2` (bench: Fan 2 is the intake) |
| number | `mist_burst` | Mist Burst | s, 3–20, default 6 (was 10 before 1.2.0): atomizer seconds per humidify cycle, delivered as pulses |
| number | `mist_pulse_on` | Mist Pulse On | s, 0.5–5 step 0.5, default 1.5: atomizer on-time per pulse within the mist phase |
| number | `mist_pulse_off` | Mist Pulse Off | s, 0–5 step 0.5, default 1.0: rest between pulses; 0 = one continuous burst (the pre-1.2.0 behaviour) |
| number | `mist_blower` | Mist Blower | %, 0–100 step 5, default 35: blower speed during the mist phase (the blower phase after it stays at 100 %) |
| number | `manual_mist_limit` | Manual Mist Limit | s, 5–600 step 5, default 60: an Atomizer switched on by hand turns itself off after this (logged). The controller's own pulses are not limited |
| sensor | `fresh_air_exchanges` | Fresh Air Exchanges | count since boot (scheduled, CO2-triggered and Fresh Air Now). Exchanges are no longer written to the Event Log |
| number | `light_start_hour` | Light Start Hour | h, 0–23, default 8: local hour the Light Hours schedule switches the Top Light on (wraps past midnight) |
| number | `co2_limit` | CO2 Limit | ppm, 0–5000 step 100, default 1500. Builds with `common/scd41.yaml` only. With Climate Control on, phase Pinning or Fruiting, and CO2 above it, a fresh-air exchange starts and is held until CO2 ≤ limit − CO2 Hysteresis (at least Fresh Air Duration, at most CO2 Exchange Max, then CO2 Exchange Gap before the next CO2 start). Ignored in Incubation; below 400 = off. With `common/recipe-co2.yaml` (and cloud recipes), the chosen Cloud Recipe sets it per phase from `pinning_co2_ppm`/`fruiting_co2_ppm` (`docs/co2-fresh-air.md`) |
| number | `co2_hysteresis` | CO2 Hysteresis | ppm, 50–1000 step 50, default 200. `common/scd41.yaml` only |
| number | `co2_exchange_max` | CO2 Exchange Max | min, 1–60, default 15: longest a CO2-driven exchange is held. `common/scd41.yaml` only |
| number | `co2_exchange_gap` | CO2 Exchange Gap | min, 1–60, default 5: rest after a CO2-driven exchange before the next CO2 start. `common/scd41.yaml` only |
| binary_sensor | `co2_exchange` | CO2 Exchange | ON while a CO2-driven exchange is held open. `common/scd41.yaml` only |
| binary_sensor | `heater_stalled` | Heater Stalled | `problem`. ON when the heater ran 45 min at 100 % without the temperature rising 0.9 °F: the controller cuts the heater until a setpoint changes or Climate Control is toggled. Also raises Heater Fault (pushed as `heater_fault`) |
| number | `rh_soft_ceiling` | RH Soft Ceiling | %RH, 85–97, default 95: stop misting above this |
| number | `rh_soft_release` | RH Soft Release | %RH, 80–96, default 92: misting may resume below this |
| text_sensor | `cloud_device_id` | Cloud Device ID | Diagnostic. The chamber's **public handle**: 12 hex characters, the first 12 of SHA-256 of a random per-chamber ID made on first boot (`common/cloud-identity.yaml`). Safe to show or share (support, screenshots): it can't be turned back into the ID, and on its own it registers nothing. The iOS app names the chamber by it when it registers for push alerts. Firmware before 1.2 showed the 32-hex ID itself. A serial erase makes a new one |
| text_sensor | `push_pairing_code` | Push Pairing Code | Diagnostic, LAN only. 8 hex characters that change every hour (HMAC of the chamber's token hash over the hour; blank until the clock is set). Apps send it with `POST /api/push/register`; the server accepts this hour's or last hour's. Seeing it proves the app was on the chamber's LAN within the hour. Firmware 1.2+ |
| text_sensor | `cloud_identity` | Cloud Identity | Diagnostic. `Registering` (first hello not answered yet), `OK`, `Replacing token`, `OK (token replaced)`, `New identity (pair remote access and push again)`, or a hint after a refused Reset. See [Cloud identity](#cloud-identity-and-token-rotation). Firmware after 1.1.0 |
| button | `reset_cloud_identity` | Reset Cloud Identity | Config, public build. Replaces the cloud token (the Cloud Device ID stays). **Only while the rear-button Pair window is open** (`Update Pairing Open` ON); otherwise refused, with a hint on Cloud Identity. Firmware after 1.1.0 |
| switch | `uv-c_lamp` | UV-C Lamp | **Eye and skin hazard; kills fungi.** OFF at every boot. Refused (stays off) while Low Water is on; switched off on low water, when an OTA upload starts and before the self-test. In builds with `common/lid-interlock.yaml`, also refused unless the lid has read shut for 500 ms, and cut the moment it opens (Event Log `UV-C blocked: lid open` / `UV-C cut: lid opened`). Apps must warn and confirm before turning it on. |
| binary_sensor | `lid_closed` | Lid Closed | Builds with `common/lid-interlock.yaml` only. ON = the lid reed reads shut (debounced: 500 ms to read shut, open at once). A missing or unplugged switch reads OFF, and UV-C stays refused (`docs/lid-interlock.md`) |
| number | `uv-c_auto-off` | UV-C Auto-Off | min, 1–60, default 15: the lamp switches itself off this long after it was turned on |
| sensor | `uv-c_remaining` | UV-C Remaining | s until auto-off, published every 1 s while on, 0 when off |
| switch | `push_alerts` | Push Alerts | Config, OFF until the owner turns it on (restored across reboots). While ON the chamber POSTs alerts to beezy.ninja, which pushes them to registered iPhones. Turning it on sends a silent check-in. See `docs/push-alerts.md` (`common/push-alerts.yaml`) |
| button | `test_push_alert` | Test Push Alert | Diagnostic. Sends a `test` alert to every phone registered for this chamber (ignored while Push Alerts is off) |
| binary_sensor | `heater_fault` | Heater Fault | ON (after 30 s) while the controller's interlock holds the heater off: the HDC1080 reading is missing or stale, or Temperature is above the 88 °F cut-off. Not ON during a sensor dry pulse. Pushed as `heater_fault` (`common/push-alerts.yaml`) |

**Load checks** (`esphome/common/load_check.h`, host tests in `esphome/tests/test_load_check.cpp`). Every load runs off the 24 V input that the INA219 measures, so a load that turns on without moving Load Current is unplugged or broken. UV-C is never part of either check.

| Load | Self-test: OK at | Passive: judged when | Passive: miss below | Bench (LOG.md) |
|---|---|---|---|---|
| Fan 1, Fan 2 | +20 mA | off → on at ≥ 50 % | +10 mA | +41 / +39 mA |
| Blower | +5 mA | off → on at 100 % | +2 mA | +6 mA |
| Atomizer | +50 mA | off → on | +25 mA | ~+190 mA |
| Heater | +30 mA | off → on (22 % stage) | +15 mA | +141 mA |

- **Self-test** (`esphome/self_test_math.h`, host tests in `esphome/tests/test_self_test_math.cpp`). Each load is bracketed: a baseline right before it switches on, a reading after it has run 5 s, and a baseline again 2 s after it is off. Every reading is the mean of eight one-shot INA219 reads 300 ms apart, and delta = on − mean(before, after), so a rail that drifts during the step (a camera booting, a light changing) cancels out instead of landing on the load.
  - The spread is the widest of the three readings' (max − min) and the difference between the two baselines. Spread above the load's limit (the "Passive: max spread" column: fans 10 mA, blower 3 mA, atomizer 25 mA, heater 20 mA) makes the verdict UNSURE unless the delta beats the threshold with the whole spread taken off. FAIL needs a quiet rail and a delta below the threshold.
  - The rail fingerprint (UV-C, Camera Power, both lights' on/brightness/colour) is taken with each reading; if it differs between readings, or a light effect/transition ran, the verdict is UNSURE (rail changed).
  - For a quiet rail the test pauses light effects (restored after), the light schedule and the auto sensor dry pulse sit out, and camera power cycles (watchdog and button) are deferred until it ends. The boot self-test also waits for 90 s of uptime and a ready (or off) camera, 180 s at most.
- **Passive check.** It never switches anything. Every 200 ms it compares the loads' state with the last poll. It judges a load only when that load alone switched on and nothing else on the rail changed: UV-C, both lights (on, brightness, colour), Camera Power. No light effect or transition may be running either.
  - The atomizer and the heater may bring a blower change or a running fan's speed step with them. The controller starts the heater and the "never heat still air" blower in the same tick.
  - Before = the readings from the 1.5 s after the previous change, frozen at the last poll. After = three one-shot reads once the new state has settled for 1.5 s.
  - If the readings spread wider than a per-load limit, the check is inconclusive, never a miss. The blower's +6 mA leaves very little margin.
  - Fans run at 3–4 % idle under Climate Control and only switch on with an exchange or by hand, so in practice the boot self-test is what checks them.
- **Events.** `<Load> not detected (+N mA)` and `<Load> detected again (+N mA)` (passive); `self-test: …` or `boot self-test: …` with one summary line.

**UV-C and stock.** Stock had a plain app switch: the V18 handler (`0x400d7820`) does `digitalWrite(18, v == 1)` while the unit is powered on (byte 76 of `0x3ffc4a74`, set by the V15 power handler). Low water and power-off cut it, and the stock self-test also turned it on. There was no timer and no automatic cycle. `uv_lamp` is that switch plus a hard auto-off, and it is never scheduled or switched on by the firmware itself. The TS-01 has no lid-interlock sensor that we know of; the opt-in reed retrofit (`docs/lid-interlock.md`) adds one. Procedure and results: `docs/uvc-bench-test.md`.

**Accepted risk (maintainer decision, 2026-10-03, confirmed 2026-10-04):** the LAN API is unauthenticated, so any *device* on the Wi-Fi can POST `uv_lamp` and energise the lamp for up to `UV-C Auto-Off` minutes. The owner chose the auto-off timer and the low-water cut-off as the safeguards, on a trusted home network, rather than a physical rear-button arm. Revisit if the chamber ever lands on an untrusted network. From the Pair-window firmware, the public build narrows this: once a phone has paired, UV-C on needs that phone's `X-AutoMyco-Key` (see [Security](#security)). *Web pages* are a different matter: that was never an accepted risk, and firmware 1.0.1 closes it (see [Security](#security)).

**Both builds and the iOS app (maintainer decision, 2026-10-04):** the public build has the same switch, so other owners can sterilise their chambers, and the iOS app has a UV-C control behind a warning dialog, with a live countdown.

Entity names and units are fixed. Apps rely on them, so any change here needs a version bump of this document.


## Security

**Problem (found 2026-10-09 on firmware 1.0.0, ESPHome 2026.9.1).** Any web page open in a browser on the LAN could drive the chamber, not just devices:
- ESPHome runs actions on **GET** (`switch`, `button`, `light`), so `<img src="http://<chamber>/switch/UV-C%20Lamp/turn_on">` on any page switched the lamp on. A GET like that carries no `Origin`, so ESPHome's origin check never ran.
- Every response carried `Access-Control-Allow-Origin: *`, so pages could also read the chamber's state.
- `/events` skipped the origin check, and there was no `Host` check, so DNS rebinding worked.

Bench probe on 1.0.0, read-only GETs only: `Origin: http://evil.example` → 200, with `Access-Control-Allow-Origin: *`.

**Fix (firmware 1.0.1): `esphome/components/web_guard`.** It's the first request handler, shared with Phytonode (one copy), and it refuses:

| Request | Answer |
|---|---|
| `Host` other than an IP literal, `<device name>` or `<device name>.local` (DNS rebinding) | 403 |
| Any `OPTIONS` (no CORS preflight is ever granted) | 403 |
| `Origin` present and not exactly `http://<Host>` (also covers `/events`) | 403 |
| `GET /<domain>/<name>/<action>` | 405, `Allow: POST` |
| `POST` with neither a same-origin `Origin` nor `X-Requested-With` (an HTML form) | 403 |

It also replaces the wildcard CORS header with `X-Content-Type-Options: nosniff`.

The LAN API stays unauthenticated (`require_auth: false`). A browser can't add `X-Requested-With` to a cross-site request without a preflight, and preflights are refused. While the setup hotspot's captive portal is active, the Host check is skipped, because phones probe with foreign names.

**Clients:**
- The iOS app sends `X-Requested-With: AutoMyco` (app versions after 2026-10-09; builds before the rename sent `ShroomChamber`). **Ship that app version before firmware 1.0.1**: older apps get `403` on every command.
- The on-device dashboard is same-origin and also sends the header.
- Scripts must add `-H 'X-Requested-With: <name>'` (see `tools/cam_led.sh`).
- A chamber reached by a router DNS name (e.g. `chamber.home.arpa`) needs `web_guard: allowed_hosts: [chamber.home.arpa]`.

**Control key for UV-C and Camera Reset Login (public build, owner decision 2026-10-09).** Once a phone has paired at the chamber (update pairing, or the `set_ota_password` API action, set the owner OTA password), web_guard refuses any request that could change these two entities unless it proves it comes from that phone:

| Entity | Needs `X-AutoMyco-Key` | Open |
|---|---|---|
| `switch` **UV-C Lamp** | every state change (`turn_on`, `toggle`, anything else) | `turn_off` |
| `button` **Camera Reset Login** | every state change (`press`) | nothing |

**Which requests.** Not a path list. web_guard decodes the URL exactly as web_server does (`url_to`: `%XX`, `+` → space, query stripped), then gates any non-GET request where *any* path segment names a keyed entity in *any* spelling: the name in any case, the object_id (`uv-c_lamp`, `camera_reset_login`), `<domain>-<object_id>`, and each of those decoded once more (double encoding). Trailing slashes, `..`, empty or extra (device) segments and other domains all still hit the gate. The only open request is UV-C with `turn_off` as the last segment. This is deliberately wider than web_server's own routing (exact name match). Host tests: `esphome/tests/test_control_key.cpp`. A config check fails the build if a keyed name doesn't exist, so a rename can't drop out of the gate. GETs never change state: GET actions are already refused (405), and `/events` is read-only. The lamp's GPIO (`uv_c`) is internal, with no web entity.

**Header format** (`esphome/components/web_guard/control_key.h`):
```
key       = SHA-256("automyco-control:" + owner OTA password)                            32 raw bytes
challenge = current state of text_sensor "Control Challenge"                             32 lowercase hex
X-AutoMyco-Key: <hex>
hex       = lowercase hex of HMAC-SHA256(key, "<METHOD> <request-target> <challenge>")    64 characters
```
- `<METHOD>` is the HTTP method (`POST`). `<request-target>` is the path exactly as on the request line, percent-encoding and query string included (`/switch/UV-C%20Lamp/turn_on`). Single spaces separate the three parts.
- Example: owner password `Abc123def456GHI789jkl012MNO345pq` (test fixture), challenge `00112233445566778899aabbccddeeff`, `POST /switch/UV-C%20Lamp/turn_on` gives `ed969c9741eb7770d127b987df978c5ec5de9a7a3bceb164c1054c8a9e9397b1`. <!-- gitleaks:allow -->
- **Single use.** The chamber accepts a header only if its MAC matches the *current* challenge (constant-time compare; hex may be either case). Then it immediately replaces the challenge with 16 fresh random bytes and publishes it. A new challenge is also generated at every boot (once the radio is up, so the RNG is a real entropy source) and whenever the owner password changes (update pairing, `set_ota_password`). So a captured header is dead once used, after a Restart, and after a re-pair. It only ever worked for its own method and path. No clock is involved: SNTP is unauthenticated, so time can't be trusted for this.
- **Apps:** read `Control Challenge` from the event stream, sign each command over it, and on a `403` GET `/text_sensor/Control%20Challenge` and retry once, because another client may have used the challenge in between. The iOS app does exactly this (`ESPHomeClient.send`).
- **Missing, wrong, or signed over an old challenge:** `403` with the body `Only the paired AutoMyco app can change this (UV-C on, restart, water and update safety settings, camera login): it needs the X-AutoMyco-Key header. Use the app or Home Assistant.` The log says so, e.g. `refused POST /switch/UV-C%20Lamp/turn_on: X-AutoMyco-Key missing, wrong or already used`.
- **Starting up:** `503` `chamber starting up: …`, logged. Web_server is up before `on_boot` loads the owner password and makes the first challenge; until then keyed actions are refused, not open. Safe mode never loads it, so keyed actions stay refused there.
- **No owner password** (loaded and empty: nobody has paired, or it was cleared with `set_ota_password ""`): both entities stay open as before, so unpaired owners aren't locked out. The chamber logs `allowed POST … without X-AutoMyco-Key: no phone has paired yet`.
- Every other entity, all reads and UV-C off never need the header. Apps may send it on every command (the iOS app signs every chamber POST).
- **The chamber's own page** (a browser, no key): once a phone has paired, switching UV-C on or pressing Camera Reset Login there shows the reason above as a toast, and the switch flips back. Turning UV-C off still works.
- **Home Assistant's native API is unaffected**: web_guard sits only in front of the HTTP web server.
- The personal build (`terrashroom.yaml`) has no keyed entities, so it is unchanged.
- iOS: `ControlKey` (`apple/AutoMyco/ShroomChamber/Transport/ControlKey.swift`, CryptoKit). The key comes from the OTA password this phone stored for that chamber, and `ESPHomeClient` signs each command over the current `Control Challenge`. A phone that didn't pair, and app builds older than this, send no header: once any phone has paired, they can't switch UV-C on or reset the camera login.
- **Limit:** headers travel in cleartext HTTP, and the challenge is public. A device that impersonates the chamber (for example by announcing its mDNS name) can relay the app's signed request to the real chamber, but that request is exactly what the user asked for: same method and path, and only once. It never reveals the password or the key.

**More keyed controls, and who may act on an unpaired chamber (security pass, firmware after 1.2.6).** The keyed list grew to every control that can disable a safety interlock, restart the chamber or change what it runs. Same header, same rules; the open action is always the safe direction:

| Entity | Keyed | Open |
|---|---|---|
| `switch` UV-C Lamp | everything else | `turn_off` |
| `button` Camera Reset Login, Restart, Camera Power Cycle, Install Firmware Update | `press` | nothing |
| `text` Remote Pairing Code, Camera Install | `set` | nothing |
| `switch` Water Lockout Override | everything else | `turn_off` |
| `switch` Atomizer Hard Stop | everything else (clearing it) | `turn_on` |
| `select` Water Sensing, Water Probe Drive; `number` Manual Mist Limit; `switch` Auto-Install Updates | every change | nothing |

The table lives in `esphome/terrashroom-public.yaml` (`web_guard: keyed_entities`). CI keeps four copies in step (`tools/test_keyed_contract.py`): the safety checker's `REQUIRED_KEYED` (rule 6: a build that keys anything must key all of these, safe direction only), the host test `esphome/tests/test_keyed_entities.cpp`, the mock's `KEYED_ENTITIES`, and the Names the app uses. The camera's own frame watchdog still power-cycles it internally; only the web button is keyed. The app signs every command, so it needs no change.

Two things need a person at an unpaired chamber (no control key yet), because there is nothing to check a header against: **UV-C on** and the **Remote Pairing Code** (pairing hands an automyco.com account remote control). Both are refused unless the rear-button Pair window is open (hold 2-4 s, then within 2 minutes), wherever the request comes from, Home Assistant included (`esphome/common/owner_gate.h`). A refused UV-C on logs `UV-C blocked: hold the rear button 2-4 s first (no paired phone)` and the switch flips back within a second. `set_ota_password` (native API) also needs the Pair window now, like update pairing: that password is the control key.

**UV-C limits (every build).** Pressing UV-C Lamp while it's lit no longer restarts the Auto-Off countdown. And the lamp can be lit for at most **60 minutes in any 24 hours**, saved across reboots (`esphome/common/uvc_budget.h`): a run is cut to what's left, and under a minute left refuses (`UV-C blocked: 60 min in 24 h used`; a lit lamp that hits it logs `UV-C off: 60 min in 24 h used`). Hours the chamber was off count once the clock is back; an unknown outage counts as zero (never frees budget early). Remote commands still only switch UV-C off.

**Home Assistant adoption (public chamber).** ESPHome's provisioning window (keyless API, all-zero key accepted, key may be set) used to reopen after every restart. It now opens only after a real power-on, or once after **Open Adoption Window** (`button`, config) pressed inside the Pair window: the chamber restarts with the window open for 30 minutes (`esphome/adopt_window.h`; host test `tests/test_adopt_window.cpp`). A chamber that already has an API key is never closed by this, so HA keeps connecting after any restart. After a power-on or Open Adoption Window the chamber also asks an AutoMyco camera to open its own window (`{"cmd":"open_api"}`, see "Camera board").

**`GET /history` rate limit.** One stream at a time, then a cooldown of twice its duration (2-60 s). Inside it: `429 Too Many Requests` with `Retry-After`. The app asks at most every 5 minutes, so it never sees one.

**Mock:** `apple/AutoMyco/mock_server/esphome_mock.py` enforces the same rules (`guard_verdict`, tests in `test_esphome_mock.py`); `--no-guard` behaves like 1.0.0; `--paired-password <pw>` acts as a paired chamber (keyed entities need a valid `X-AutoMyco-Key` over the mock's `Control Challenge`; same parsing and single-use rules, `KeyGate`).

**Verify on hardware after flashing.** Use a harmless entity, never UV-C. `H=<chamber>.local`.
```sh
curl -s -o /dev/null -w '%{http_code}\n' "http://$H/sensor/Temperature"                                   # 200
curl -s -o /dev/null -w '%{http_code}\n' "http://$H/light/Top%20Light/toggle"                              # 405
curl -s -o /dev/null -w '%{http_code}\n' -X POST -H 'Content-Length: 0' "http://$H/light/Top%20Light/toggle"  # 403
curl -s -o /dev/null -w '%{http_code}\n' -H 'Origin: http://evil.example' "http://$H/sensor/Temperature"  # 403
curl -s -o /dev/null -w '%{http_code}\n' -H 'Host: evil.example' "http://$H/sensor/Temperature"           # 403
curl -s -o /dev/null -w '%{http_code}\n' -X OPTIONS "http://$H/light/Top%20Light/toggle"                  # 403
curl -s -D - -o /dev/null "http://$H/sensor/Temperature" | grep -ci access-control                          # 0
curl -s -o /dev/null -w '%{http_code}\n' -X POST -H 'Content-Length: 0' -H 'X-Requested-With: curl' "http://$H/light/Top%20Light/toggle"  # 200 (toggles the light; run twice)
```
`web_guard` reaches into ESPHome internals (`web_server_idf::DefaultHeaders`, handler order), so repeat these checks after every ESPHome upgrade.
## Cloud identity and token rotation

Each chamber makes a random **device ID** (32 hex) and **token** (64 hex) on its first boot (`common/cloud-identity.yaml`). Every cloud request (`${cloud_base_url}`) carries `X-Device-Id: <id>` and `Authorization: Bearer <token>`. The server keeps only SHA-256(token) and binds it on first use (`site/lambda/device-auth.mjs`). Neither value is ever shown; the chamber shows the public handle (Cloud Device ID) and the hourly Push Pairing Code instead.

**Problem.** The token lives in NVS, and anyone with a serial cable can read it. Before rotation, a previous owner who did that could keep posting telemetry, push alerts and health reports as the chamber after selling it, for as long as they liked. About 1,040 orphaned TS-01s will change hands second-hand, so this is a realistic case.

**Rotation (firmware after 1.1.0).** The chamber replaces its token, and keeps its ID, after something only a person at the chamber can do:

| Trigger | Where |
|---|---|
| A Remote Pairing Code is accepted | `common/remote-access.yaml` |
| A phone pairs for updates in the rear-button Pair window (`Update Pairing Fingerprint` published) | `terrashroom-public.yaml` |
| **Reset Cloud Identity** pressed while the Pair window is open | `terrashroom-public.yaml` |
| A Remote Pairing Code is **refused with 401** (a previous owner already rotated the token away with a copy) | `common/remote-access.yaml`: the rotation is refused too, so the chamber starts over with a new ID; the code isn't used up, so enter it again |
| Serial erase (factory reset) | already gives a brand-new ID and token |

A previous owner who rotates the token away *after* the sale leaves the chamber getting 401 everywhere (Remote Status shows "Refused"). Any of the triggers above then recovers it with a new ID, so the copy holder ends up with an ID nothing uses.

So a new owner who onboards the iPhone app (update pairing) or pairs remote access has already killed any copy of the old token. Someone who wants neither can hold the rear button 2–4 s and press Reset Cloud Identity.

Protocol, `POST /api/device/rotate`, device auth with the **current** token, body `{"next_hash": "<SHA-256 hex of the new token>"}`. The new token itself never leaves the chamber. Firmware after 1.2.6 adds `"reason"`: `"update_pair"` (update pairing), `"remote_pair"` (a Remote Pairing Code), or `"reset"` (**Reset Cloud Identity**, after which the server also drops the ID's push subscriptions, so a previous owner's phone stops getting alerts). It is saved with the pending token, so a replay after a lost answer or a power cut sends the same reason; when several triggers queue before one rotation completes, the strongest is sent (reset > remote_pair > update_pair). Servers that don't know the field ignore it.

| Answer | Meaning | Chamber does |
|---|---|---|
| `200 {ok, handle, rotated: true}` | swapped in one conditional write; the old token is dead from now on | token := new |
| `200 {..., rotated: false}` | this exact rotation had already happened (a retry after a lost answer) | token := new |
| `400` | `next_hash` missing, not 64 lowercase hex, or the hash of the current token | backs off (a firmware bug) |
| `401` | the current token isn't this ID's any more (a copy holder rotated first) | starts over: new ID **and** token, fresh hello |
| `429`, `Retry-After: 600` | at most one rotation per 10 min per chamber | keeps the pending token, tries again in 10 min |
| other / no answer | | backs off 1, 2, 4 … 60 min, keeps the pending token |

A chamber the server has never seen (its first hello never got through) binds `next_hash` straight away, as any first request would.

**Why the ID stays.** It's the key for push subscriptions, the remote-access owner and its history, and the public handle the app and support know the chamber by. Changing it on every pairing would break all of those for the rightful owner. Only on a 401 does the chamber give the ID up, because then a copy holder already controls it.

**Power-cut safety.** The chamber keeps the new token in a second saved global (`cloud_token_next`) and moves it in three steps, each on flash before the next starts:
1. Make the new token and save it as *next*. Only then send `next_hash`. (The old token stays the token.)
2. On a 2xx: token := next, keeping *next*, and save.
3. Clear *next*.

Flash therefore never holds the old token without the new one. A cut between the server's swap and step 2 replays the same rotation at boot (20 s after boot, ahead of other cloud requests), and the server answers `200 rotated:false` because it still remembers the hash it replaced (`prevHash`). `next == token` at boot means step 3 was lost, and it's finished then. A cut in the moment between the trigger and step 1 reaching flash loses the rotation and nothing else (both sides keep the old token; Cloud Identity doesn't say "OK (token replaced)"), so press Reset again. `esphome/tests/test_token_rotation.cpp` cuts power before every step, with and without a lost answer. Each step is written with `ts_persist_now` (`common/persist_now.h`), because a plain `global_preferences->sync()` would write a restored global's old value.

**What follows the token.** The Push Pairing Code (recomputed at once), the camera's frame-upload key (the chamber re-sends it over the camera wire and reports the new `ck` within about a minute), and every authenticated request.

**Rotation reasons (server since 2026-10-13; firmware 1.2.7 and later send it).** The body may carry an optional `reason`:

```json
{"next_hash": "<SHA-256 hex of the new token>", "reason": "reset"}
```

| `reason` | Sent when | Server does, besides the swap |
|---|---|---|
| `"reset"` | **Reset Cloud Identity** pressed in the rear-button Pair window | forgets every phone registered for this chamber's push alerts (the Push Pairing Code changes with the token, so only someone at the chamber can register again) |
| `"remote_pair"` | a Remote Pairing Code was accepted | nothing more (the pairing itself already moved remote access and its phones) |
| `"update_pair"` | a phone paired for updates in the Pair window | nothing more |
| missing, or anything else | firmware 1.2.0–1.2.6, or a reason this server doesn't know | nothing more (treated as `"unspecified"`) |

Answers are unchanged (`{ok, handle, rotated}`, same status codes), so 1.2.0–1.2.6 keep working without sending it. A replayed `"reset"` (`rotated: false`, after a lost answer) clears the phones again, which is harmless. Send the reason on the replay too: the server only acts on what the request says. Matching is exact and lowercase.

**Not covered.** Rotation doesn't unpair remote access by itself. A new owner moves remote access by pairing it to their own account (which also rotates the token); that pairing tells the previous owner (a "moved to another account" push to the chamber's phones, then an email to the account's verified address) and forgets the previous owner's phones. Without a `"reset"` reason (firmware before 1.2.7) a plain rotation keeps the push phones.

## Cloud grow recipes (`common/cloud-recipes.yaml`)
Species presets published as data, so new species need no firmware release. The chamber GETs `https://beezy.ninja/recipes/v1/index.json` (public, static, TLS-verified, no device ID or token sent) 2 min after boot, every 24 h and on **Refresh Cloud Recipes**, in a background FreeRTOS task so the controller never waits on the network. Schema: `site/public/recipes/v1/schema.json`.

- **Checked on the chamber.** `common/cloud_recipes.h` re-checks every value against the number entities' limits (temperature 55–85 °F, RH 50–99 %, idle airflow 1–64/255, Light Hours 0–16, fresh air 5–240 min / 10–600 s) and clamps strays. A stage at or above `max_safe_temp_f`, with RH outside 0–100, a missing or non-numeric field, or bad stage order rejects that recipe. A file with no good recipe, the wrong `schema`, or 8 KB or more is ignored. Unknown fields are ignored, so schema 1 can grow new optional fields.
- **Cached in flash.** The last good set (up to 24 recipes × 4 stages) survives reboots and works offline; it is rewritten only when it changes. The chosen recipe is kept by name, with the stage last applied.
- **Stages.** A recipe has 1–4 stages, each starting on a `from_day` (the first on day 1). The chamber applies the last stage whose `from_day` ≤ Grow Day, when the stage changes (checked every minute and on Start Grow). With no grow running, a newly chosen recipe applies its first stage. Each stage sets the temperature and humidity setpoints and the idle airflow, and Light Hours / Fresh Air Interval / Fresh Air Duration only if it lists them. If a newer index changes the stages of the chosen recipe, the stage in force is re-applied; if the recipe is withdrawn, the select goes back to `None` and the setpoints stay.
- **Never UV-C.** No recipe field maps to the UV-C lamp, and the package has no code path to it.
- **Built-in presets** (`grow_preset`) keep the stock values and need no network. The published index has one recipe per built-in preset, same names and order. Since revision 2, seven of them are corrected against cultivation references (`"source": "audited"`: Shiitake, Pink Oyster, Blue Oyster, Chestnut, King Trumpet, Snow Oyster, Nameko; see `docs/species-presets.md`), so the cloud recipe of those names differs from the built-in preset. The rest are identical (`site/test/recipes_test.py` checks both). Recipes also carry optional per-species phase targets (`incubation_temperature_f`, `pinning_co2_ppm`, `fruiting_co2_ppm`). Firmware 1.x ignores `incubation_temperature_f`; the CO2 targets are applied only by builds with `common/recipe-co2.yaml` (needs the SCD41 package), and ignored by every other build, the public one included.

## Firmware updates (native OTA, TCP 3232)
`web_server` OTA is off. Updates use ESPHome's native OTA protocol on **TCP 3232**, protected by `ota_password` (personal build) or, on the public build, by the password the owner sets with the `set_ota_password` API action (plaintext OTA is locked until then; the API key holder can always use encrypted OTA). The iOS app implements it (`apple/AutoMyco/ShroomChamber/Firmware/ESPHomeOTA.swift`, written against ESPHome 2026.9.1 `espota2.py` and `ota_esphome.cpp`); `apple/AutoMyco/mock_server/ota_mock.py` is a mock of the device side.

- **Installed version.** The `Firmware Version` text sensor above. Older builds don't have it, but every build advertises `project_version` (and `project_name` = `community.terrashroom-ts01`) in the TXT record of its `_esphomelib._tcp` mDNS service.
- **Release version.** `site/scripts/publish_firmware.py --version` must equal `esphome.project.version` in the build. For main-board images the script checks the image's `Project community.terrashroom-ts01 version <v>` string to enforce that.
- **Handshake.** Magic `6C 26 F7 5C 45` → `00 <ota version 1|2>`; client features (`0x02` SHA-256 auth, `0x04` extended protocol) → `48 <server flags>` (or one legacy byte `40`/`46`); `02` + 64-hex nonce → client sends a 64-hex cnonce then `hex(sha256(password + nonce + cnonce))` → `41`. ESPHome 2026.9 accepts only SHA-256 auth (older firmware asked for MD5: `01` + 32-hex nonce). Then the OTA type byte `00` (app, extended protocol only), 4-byte big-endian size → `42`, 32-hex MD5 of the image → `43`.
- **Data.** 8192-byte blocks; protocol v2 acks every block with `47`. Then `44` (received) and `45` (image verified, boot slot switched), and the client sends `00`. The device reboots. Compression isn't used: the ESP32 backend doesn't support it. Any byte `0x80`–`0x94` or `0xFF` is an error code.
- **Why an interrupted update can't brick the chamber.** The image goes into the *inactive* app slot. The boot slot only changes in the step that answers `45`, after the MD5 and the image check pass. A drop, a wrong password, a bad image or a timeout (105 s with no data) leaves the running firmware untouched. If the connection drops after the whole image was sent, the device may already have switched, so apps must check the version after the reboot instead of retrying blindly.
- **Safety during the upload.** The device's main loop is blocked while it writes flash, so `ota: on_begin` switches off the heater, atomizer and UV-C lamp first.

## Camera link and camera install (main board, `common/camera-link.yaml`)

| Domain / Name | Notes |
|---|---|
| `text` **Camera Install** | Write `"<version> <sha256>"` (a published `board: camera` release) to install it on a stock camera. It always reads back empty |
| `text_sensor` **Camera Install Status** | `idle`, `starting camera`, `sending Wi-Fi`, `requesting firmware`, `downloading`, `restarting camera`, `installed`, `already installed`, `failed: <reason>` |
| `text_sensor` **Camera Firmware** | The camera's last ready line: `camera/0.1.7.09` (stock) or `automyco-camera/<version>` |
| `text_sensor` **Camera Address** | The AutoMyco camera's IPv4 once it's on Wi-Fi |
| `button` **Open Adoption Window** | Public build. Inside the Pair window only: with no API key yet, the chamber restarts with its Home Assistant provisioning window open for 30 min; an AutoMyco camera is asked to open its own (`{"cmd":"open_api"}`). With a key already set, only the camera is asked. Outside the window: refused, logged |
| `button` **Camera Power Cycle** | Hard camera restart: camera power off 10 s, then on (un-wedges the camera sensor; the camera's frame watchdog asks for the same). Refused while an install runs; an install is refused while a power cycle is running (`failed: camera power cycle running, try again in 10 s`) |
| `button` **Camera Reset Login** | Clears the camera's login; the camera then reboots and opens its own 30-min claim window. Only within 30 min of the chamber's power-on, or (public build) within 2 min of a 2–4 s rear-button hold (the Pair window). Refused: `failed: power-cycle the chamber or hold the rear button 2-4 s, then reset the camera login`. Once a phone has paired, the public build also needs `X-AutoMyco-Key` (see [Security](#security)) |
| `sensor` **Camera Power Cycles** (`camera_power_cycles`) | Diagnostic. Count of camera power cycles from every path (button, the camera's watchdog, each install round), kept across reboots |
| `text_sensor` **Camera Last Cycle Reason** (`camera_last_cycle_reason`) | Diagnostic. Why the camera was last power-cycled, e.g. `Camera Power Cycle button`, `camera: no valid camera frame for 60 s`, `install round 2`. Empty until the first cycle after boot. Camera power cycles are no longer written to the Event Log |

The stock-camera timelapse package (`common/stock-camera-timelapse.yaml`: Timelapse, Timelapse Interval, Timelapse Flash, Camera Status, Timelapse Frames Since Boot) is opt-in from 1.2.0: it's commented out in `terrashroom-public.yaml` and not in `terrashroom.yaml`. Apps must not assume those entities exist.

The full flow, the camera link protocol and the failure reasons are in `docs/camera-install.md`.

## Camera board (second device)
The chamber's camera board runs its own ESPHome firmware: the public build `esphome/camera-public.yaml`, which owners install from the iOS app (`docs/camera-install.md`), or the owner's `esphome/camera.yaml`. It's a separate host on the LAN, with mDNS name `automyco-camera-<6 hex>.local` (for example `automyco-camera-a1b2c3.local`; `terrashroom-camera-<6 hex>` before 2026-10-09). It advertises `_esphomelib._tcp` and `_http._tcp`, so apps should tell it apart from the chamber by the `automyco-camera` (or older `terrashroom-camera`) name prefix.

| Endpoint | Purpose |
|---|---|
| `GET http://<camera>:8080/stream` | Live MJPEG stream (`multipart/x-mixed-replace; boundary=frame`) |
| `GET http://<camera>:8080/snapshot` | One JPEG snapshot |
| `GET /events`, `POST /<domain>/<Entity Name>/<action>` on port 80 | Same web_server v3 REST + SSE contract as the chamber (above) |
| `GET`/`POST http://<camera>:8080/claim` | Public build only, no auth: `{"claimed","open"}`, and one-time login setup `{"username","password"}` (200 / 409 claimed / 403 window closed / 400). The POST must be `Content-Type: application/json`, carry no `Origin`, and name the camera in `Host` (IP literal or `<name>[.local]`), else 403; also 403 while the camera is still on its placeholder network. See `docs/camera-install.md` |

Camera entities:

| Domain / Name | Notes |
|---|---|
| `switch` **Camera Light** | 1.2.0+ camera firmware: the LED next to the lens lights only while someone watches `/stream` or a `/snapshot` is taken, and only when this is ON. Restored across reboots, default OFF |
| `light` **Camera Flash** | Older camera firmware only (internal from 1.2.0). The LED itself: `turn_on?brightness=0-255`, `turn_off`. The app turns it on only while live video is visible or a snapshot is taken |
| `switch` **Upload Frames** | Personal build only. Uploads a frame to your own S3 bucket every `frame_interval` (default 5 min). Restored across reboots, default ON |
| `sensor` **Frames Uploaded** / **Frame Upload Failures** | Personal build only. Counters since boot |
| `switch` **Light Timelapse Frames** | Both builds (the public build's remote frames, `common/camera-remote.yaml`, too). Config, restored across reboots, default ON. Lights dark uploaded frames with the camera's LED: the sensor's exposure index at or above `dark_exposure` (default 1000) means dark, so the LED fades up for about 1.2–1.5 s around the capture (never more than 2.5 s) and then goes back to how it was. If the LED is already on (a lit `/stream`), the frame is taken as is. Independent of **Camera Light**, which only covers `/stream` and `/snapshot` |
| `sensor` **Frame Exposure** | Diagnostic. The OV2640 exposure index (auto-exposure lines × analog gain) read before the last upload. Unknown until the first upload, or when the sensor can't be read (that frame is then lit) |
| `sensor` **Frames Lit** | Diagnostic. Uploads taken with the LED, since boot |
| `sensor` **Camera WiFi Signal** | dBm |
| `button` **Camera Restart** | |

**Auth:** every camera endpoint (stream, snapshot, REST and SSE) requires HTTP Basic auth with the login the app claimed through `POST :8080/claim` (both camera builds, 1.2.0+; nobody types it). Cameras still on an older fixed-login build use `camera_web_username` / `camera_web_password` from that build's `secrets.yaml`. A 401 carries `WWW-Authenticate: Basic realm="AutoMyco Camera"`. Apps should send the header preemptively and keep the credentials in the Keychain. OTA is native and password-protected only. The camera never holds AWS keys. It uploads through the frame-ingest Lambda with a bearer token (`cloud/frame-ingest`).
