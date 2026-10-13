# CoreS3 Thread Border Router + Matter

Firmware that turns an [M5Stack CoreS3 Thread BR](https://docs.m5stack.com/en/core/CoreS3_Thread_BR) into a Thread 1.4 border router you can drive from the touch screen. It does both halves of ePSKc: it can **fetch** credentials from another border router, and **hand its own out** to a commissioner. It also serves the OpenThread Border Router REST API and exposes the standard Matter 1.5.1 Thread Border Router device type (`0x0091`), so Home Assistant can manage it as both an OTBR and a Matter node.

## Web flashing

Anyone with Chrome or Edge can flash straight from the browser, no installs:

```
https://espressif.github.io/esp-launchpad/?flashConfigURL=https://raw.githubusercontent.com/Poshy163/cores3-epskc-commissioner/main/launchpad.toml
```

[ESP Launchpad](https://espressif.github.io/esp-launchpad/) reads `launchpad.toml` and offers two deliberately different images:

* **Factory install (resets settings)** writes the bootloader, partition table and app as one complete image. Use it for stock M5Stack firmware, recovery, or a changed bootloader or partition layout. Because the image spans the NVS partition, it clears saved Wi-Fi, Thread credentials and settings.
* **Update (keeps settings)** writes only the app at `0x10000`. Use it only when the device already runs this project with a compatible bootloader and the layout in `partitions.csv`; it leaves NVS untouched.

The first upgrade from a pre-Matter build must also write the new partition table, which adds the dedicated `matter_nvs` partition. To keep the existing Wi-Fi and Thread dataset, use the sparse PC flash described under **Building**; an app-only browser update cannot migrate the layout, while the merged factory image resets NVS.

The GitHub Actions workflow rebuilds both choices on every source push. There can be a short delay before its follow-up firmware commit reaches `main`, so wait for the build workflow to finish before using Launchpad for a just-pushed change.

## Hardware

One M5Stack CoreS3 Thread BR (SKU K149). The ESP32-S3 runs this firmware. The on-board ESP32-H2 keeps its stock RCP firmware and never gets touched.

Flashing replaces M5Stack's stock border-router firmware. To go back, use M5Burner's factory restore.

## Wi-Fi

Two ways to get the device online:

* On the screen: **Settings > Wi-Fi**, pick your network from the scan, type the password on the keyboard. Saved to flash, it survives reboots, app-only Launchpad updates and the sparse local flash command. **Factory install**, `erase-flash`, factory reset and **Leave** erase it.
* Over the console: open a 115200 serial terminal (ESP Launchpad has one built in) and run:

  ```
  wifi "My Network" "mypassword"
  ```

  Quote both arguments. The console splits on whitespace, so an unquoted SSID or password containing a space arrives truncated.

### Staying online

A border router is only useful while its backbone is up, so the device keeps trying to reconnect for as long as it holds credentials. Attempts are immediate for the first three, then back off 1, 2, 5, 15 and 30 seconds to a ceiling of one attempt a minute, which it will sustain indefinitely. A ten-second watchdog re-arms the ladder if an attempt is ever lost. Nothing about an outage is fatal: an access point that reboots, changes channel or is switched off for a week is picked up again when it returns.

A new password is tested in RAM and only written to flash once it has actually connected, so a reset or a power cut while a wrong one is being tried can no longer destroy the credentials that were working. A failed attempt leaves the previous network in flash untouched.

`status` reports the driver's configured SSID separately from the live connection. This is not a readback of NVS: during a trial or after a failed save it can differ from persisted credentials. A failed rollback keeps reconnection held until a new explicit join or reboot; the watchdog never retries an unverified candidate as though it were the saved network. `link` also shows the disconnect count and the reason code the access point last sent.

`link drop [n]` forces `n` rapid disconnects to simulate an outage, and `link supervisor off` restores the old give-up-after-five-attempts behaviour. Both are diagnostics, neither is persisted, and the supervisor is on again at every boot.

## Joining someone else's network

1. Ask your border router for an ephemeral key. In Home Assistant that's the Thread panel once ePSKc support lands; today you can run `ot-ctl ba ephemeralkey start <9 digits> 300000` inside the OTBR add-on container.
2. **Scan**, pick your router from the list.
3. Type the code, or press **QR** and hold the code up to the camera.
4. Watch it join. Keys are single use and expire, so if the join fails, mint a fresh one.

Failures name the stage that went wrong rather than sending you to the serial log.

## Running its own network

**New network** generates fresh credentials (random key, PSKc, extended PAN, channel), forms the network and becomes leader. It appears in Home Assistant's Thread panel as a separate network named `CoreS3-XXXX`. Handy for validating things against a mesh that is yours alone.

**Network** shows the current role and the information that is valid for it: a child's parent RLOC, signal and link quality, or a router/leader's directly attached children. Its actions are:

* **Activity** shows attachment time, parent/direct-child details, the local router table and radio frames sent/received during a rolling 60-second window. **Refresh mesh** asks every reachable router for a best-effort topology snapshot. Counts deliberately say "seen": sleepy children, stale tables or a router that does not reply can make this lower than the true network population.
* **Share** puts a 9-digit ePSKc code on screen and advertises `_meshcop-e._udp`, so a commissioner can pull the credentials from this device.
* **Dataset** renders the Active Dataset TLVs, the form Home Assistant accepts for "add network".
* **Forget network** erases the stored credentials.

Anything destructive needs a second tap to confirm.

## REST API for Home Assistant

The device serves the OTBR REST API on port 8081, so you can add it in Home Assistant as an OpenThread Border Router pointed at `http://<device-ip>:8081`.

| Endpoint | Methods |
|---|---|
| `/node` | GET |
| `/node/ba-id`, `/node/ext-address`, `/node/ext-panid`, `/node/network-name`, `/node/rloc16` | GET |
| `/node/state` | GET, PUT or POST (`enable` / `disable`) |
| `/node/dataset/active` | GET (hex TLVs with `Accept: text/plain`), PUT |
| `/node/dataset/pending` | GET |
| `/node/ba-epskc/state` | GET, PUT |
| `/node/ba-epskc/key` | GET, POST, DELETE |
| `/logs` | GET (see [Logs over Wi-Fi](#logs-over-wi-fi)) |
| `/power/history` | GET (see [Settings](#settings)) |

The `ba-epskc` endpoints follow [python-otbr-api #267](https://github.com/home-assistant-libs/python-otbr-api/pull/267). They can be switched off at **Settings > Thread > ePSKc over REST**, which *unregisters* them rather than returning an error, so the API becomes indistinguishable from a build without ePSKc support. That makes it easy to test both branches of a client.

Plain HTTP with no authentication, exactly like ot-br-posix: anyone who can reach port 8081 can read the Active Operational Dataset, which contains the network key. Keep it on a trusted network.

### Logs over Wi-Fi

Every line the firmware logs, including Matter and OpenThread, is also kept in PSRAM: the last 1,024 lines of this boot and the last 1,024 of the previous one. `GET /logs` serves them, so seeing what the device has been doing no longer needs a USB cable.

| Query | Meaning |
|---|---|
| `boot=previous` | the boot that ended in the last panic, watchdog or software reset |
| `after=<seq>&limit=<1..200>` | page by sequence number, like `/power/history` |
| `format=text` | plain lines instead of JSON, for `curl` |

```
curl "http://<device-ip>:8081/logs?boot=previous&format=text"
python tools/logtail.py --url http://<device-ip>:8081 --follow --output cores3.log
```

`logtail.py` polls every 2 seconds and prints new lines; `--previous` dumps the previous boot instead. If the device reboots while it is following, it fetches the lines the old boot wrote after the last poll from the retained copy, then reports how the new boot started, so a crash does not hide its own last words. Lines overwritten before they were read are reported, not skipped silently.

The previous boot survives because the ring sits in PSRAM that start-up leaves alone, and each line is pushed out of the data cache as it is written (the ESP32-S3 does not flush that cache on a reset). A power cut or a brownout loses it, and so can flashing different firmware, which may move the ring. `reset_reason` (in the text form, the header line) says how the previous boot ended: `panic`, `task watchdog`, `software`, `power on` and so on. The panic handler prints its backtrace directly rather than through logging, so the backtrace itself is not captured; decode one from a USB capture with the ELF SHA-256 that `status` reports.

What is captured follows the console verbosity set with `log`: the default quiet profile keeps warnings and errors from every component and this firmware's own info lines. Lines are cut at 179 bytes. Capture costs one extra copy per line and 384 KB of the 8 MB PSRAM; it uses no internal RAM and never writes flash. **Settings > Thread > Logs over REST** (or `rest logs off`) unregisters the endpoint while capture carries on, so a crash log is still there if you switch it back on. The firmware does not log the network key, PSKc, Wi-Fi password or Matter pairing codes: `reveal` and `matter code` print to the USB console only, outside the captured log.

## Matter and Home Assistant topology

The Matter node has a standard Root Node plus a Thread Border Router endpoint with Thread Network Diagnostics and Thread Border Router Management (including PAN Change). It identifies itself as vendor **M5Stack**, product **CoreS3 Thread BR**, with the firmware's git revision as its software version, and advertises device type `0x0091` while commissionable. Matter and MeshCoP use the same saved device name: it is the Matter NodeLabel (what Home Assistant names the device), the TBR Management `BorderRouterName`, and the `_meshcop._udp` instance on the LAN. Changing it under **Settings > Name** or with `name` republishes all three without a reboot. The Wi-Fi DHCP hostname is separate and fixed at build time: `cores3-thread-br` (`CONFIG_LWIP_LOCAL_HOSTNAME`), replacing ESP-IDF's generic `espressif`.

The `_meshcop._udp` record itself is corrected by the firmware. ESP-IDF's prebuilt border-router library publishes it with the Thread role stuck at "detached", the Active Timestamp shifted by two bytes and the device's own OMR prefix instead of the network's favoured one; the firmware re-applies OpenThread's own encoding of the record whenever it changes, so the entry matches what ot-br-posix and the Apple and Google routers on the same network advertise. While a share code is active, `_meshcop-e._udp` is published under the same instance name with the same record.

This development build uses the Connectivity Standards Alliance test vendor/product range and Espressif's example device-attestation credentials. It is interoperable Matter firmware, but it is not a CSA-certified commercial product. Home Assistant Matter Server 9.0 and later therefore needs its **Test-Net DCL** option enabled before commissioning it.

On the serial console:

```text
matter status
matter code
matter open 900
```

Use the manual code or QR payload from `matter code` in Home Assistant's Matter add-device flow. An uncommissioned device advertises on the LAN and over BLE; after it has a fabric, `matter open` follows the Matter requirement to open subsequent commissioning on the operational network only. Once commissioned, Matter Server can interview endpoint `0x0091` and include the border router and its diagnostics in the Matter network topology.

### Matter battery entities

The node also exposes a Power Source endpoint with Battery and Rechargeable
features. Home Assistant discovers **Battery**, **Battery voltage**, and
**Battery charge state** under the existing Matter device. This operates
independently of the PC recorder; the OTBR REST integration alone does not
provide these entities. Home Assistant's [Matter sensor mappings](https://github.com/home-assistant/core/blob/2026.9.2/homeassistant/components/matter/sensor.py)
support these standard attributes.

Battery percentage uses the same gauge samples as the display and is encoded
in Matter's half-percent units (100% = 200). Missing, invalid, voltage-estimated,
or stale (60 seconds without a fresh sample) percentages become unknown.
Cell voltage has independent validity and is sent at most once a minute;
validity transitions are immediate. Percentage and charge-state changes are
published from the existing 10-second samples, including with the screen off.
The charger state distinguishes charging, full charge and not charging.
The required non-nullable charge-level flag uses warning below 15% and critical
at 5% or below, retaining its last known value when the gauge is unknown.
Live attributes are not persisted to flash.

For an already-paired device, refresh its Matter interview if the new endpoint
is not discovered after the firmware update. Preserve the existing pairing;
removing the device or factory-resetting it is not required by this change.

## Settings

Wi-Fi, screen brightness and sleep timeout, a live power page, Thread options (router preference, fixed channel for new networks, share-code lifetime, REST toggles), device name, an About page with firmware hash and uptime, plus Reboot and Factory reset.

The power page reports battery percentage, voltages, PMIC and ESP32 temperatures, charge state, and a discharge rate estimated from the gauge. There is no live wattage: the AXP2101 has no current-sense channel.

The CPUs scale between 80 MHz when idle and the configured maximum (normally
160 MHz) during work, keeping the peripheral bus at 80 MHz. Automatic light
sleep is disabled so the H2 radio, USB console and border routing stay available.
On battery, the screen dims to at most 20% after 30 seconds without a touch,
then turns off at the saved sleep timeout. Touching restores the saved brightness;
camera and sharing screens stay bright. Live labels stop refreshing while the
screen is off. These changes do not overwrite saved screen preferences.

Battery percentages marked `~` in the header or `est` on the power page are
coarse voltage estimates, affected by load and cell condition. This fallback is
withheld on USB because charging voltage is unsuitable for the discharge curve.
A valid zero gauge reading near empty stays zero. Unknown telemetry is not
reported as a negative percentage. Time remaining needs at least ten minutes
and a two-point gauge drop; charging, missing data, long sampling gaps and upward
gauge corrections invalidate the history. It is an extrapolation of recent use,
not a calibrated runtime guarantee.

For longer discharge tests, `GET /power/history?after=0&limit=120` on the REST
port returns paged battery telemetry. `boot_id` identifies a boot, `seq` orders
its samples, and `oldest_seq`/`latest_seq` expose retention bounds. Up to 8,640
samples are buffered in PSRAM (24 hours at the normal 10-second interval).
Reconnecting USB does not clear this log. Reboot or complete power loss does;
the PC recorder provides durable storage. Logging follows the UI's power timer,
including while the screen is asleep; a stalled UI or missing samples are
visible through sample age and timestamps.

Run `tools/battery_record.py --url http://<device-ip>:8081 --output <path>.csv
--keep-awake` using Python on Windows. It collects over Wi-Fi every 30 seconds,
flushes each sample to disk, resumes from existing CSV without duplicates, and
records reboots/missing samples in a companion `.events.jsonl`. A `.status.json`
shows the most recent reading, sample age, error, saved-row counts and process
ID. It runs for 24 hours by default (`--hours` changes this); `--keep-awake`
prevents automatic system sleep while it runs, without changing power-plan
settings. Manual sleep/shutdown and network loss can still interrupt collection.

CSV timestamps are UTC estimates anchored to PC receive time; `uptime_ms`
is the device's monotonic sample time for discharge calculations. `valid=false`
marks a failed PMIC read; unknown numbers are blank in CSV/null in JSON. Screen
percentage is the requested backlight setting, not a measured light level.
The recorder never fetches Thread credentials or changes device settings.

Wi-Fi power management follows the power source. On USB the radio never dozes, which keeps backbone round trips in the low milliseconds rather than the 100 to 300 ms that modem sleep adds to every packet routed through a border router. On battery it returns to modem sleep automatically.

## Console commands

| Command | Does |
|---|---|
| `wifi <ssid> <pass>` | connect, and save to flash only once the connection succeeds |
| `link [drop [n]\|retry\|supervisor on\|off]` | Wi-Fi link state, disconnect count and last reason code; `drop` simulates an outage |
| `reboot` | restart the device |
| `status` | Wi-Fi, IP and firmware version |
| `join <ip> <port> <code>` | run the ePSKc exchange from the console |
| `newnet` | form a new Thread network |
| `share [stop\|state]` | hand out an ePSKc code |
| `thread` | attachment, parent/direct children, local routers and traffic counters |
| `thread scan` | run a best-effort mesh topology scan and show devices seen |
| `forget` | erase Thread credentials |
| `rest [start\|stop\|epskc on\|off\|logs on\|off]` | OTBR REST API, and its switchable endpoints |
| `logs [cur\|prev [lines]]` | where `/logs` stands, or the last lines of this or the previous boot, over USB |
| `power` | voltages, temperatures, discharge rate |
| `settings` | show persisted settings |
| `batt` | battery %, voltage, charge state |
| `name <name>` | set the shared Matter/MeshCoP border-router name; republished immediately |
| `matter [status\|code\|open [seconds]]` | inspect or open Matter commissioning |
| `camtest` | grab frames, print stats and ASCII art |
| `reveal 1` | unmask the network key in output. It prints secrets, so leave it off |
| `log [quiet\|<level> [tag]]` | console verbosity. Boots `quiet`: warnings and errors from every component, this firmware's own tags at info. `log info` restores everything, `log info OPENTHREAD` one tag |

`status` also reports uptime, heap headroom, ESP-IDF version and the application ELF SHA-256 for matching crash symbols. `thread` prints the version string of the ESP32-H2 RCP firmware.

If either NVS partition cannot initialize, boot preserves its contents instead of automatically erasing provisioning. Back up flash before an explicit recovery or factory reset.

## Building

See [CLION.md](CLION.md) for a local ESP-IDF setup. For day-to-day work `build-native.ps1` uses that local install, reuses `build/` and ccache, so a rebuild after a source edit takes seconds. Its `-Port` writes only the app partition, which keeps Wi-Fi, Thread and Matter data; add `-Full` to write the bootloader and partition table too, or `-Monitor` to open the console afterwards:

```powershell
.\build-native.ps1 -Port COM5
```

`build.ps1` uses the same ESP-IDF v5.5.4 release as CI and builds in Docker, so it stays reproducible regardless of what is installed. Supplying `-Port` flashes the bootloader, partition table and app as separate ranges, so it does not overwrite NVS:

```powershell
.\build.ps1 -Port COM5
```

CI publishes four files under `firmware/`:

| File | Offset | Purpose |
|---|---:|---|
| `bootloader.bin` | `0x0` | Sparse/manual flashing |
| `partition-table.bin` | `0x8000` | Sparse/manual flashing |
| `epskc-commissioner-app.bin` | `0x10000` | Launchpad **Update (keeps settings)** and sparse/manual flashing |
| `cores3-epskc-commissioner.bin` | `0x0` | Complete merged image for **Factory install** and compatibility |

To preserve settings when flashing the published components manually, write the first three files at their listed offsets and do not run `erase-flash`:

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  0x0 .\firmware\bootloader.bin `
  0x8000 .\firmware\partition-table.bin `
  0x10000 .\firmware\epskc-commissioner-app.bin
```

The merged image necessarily spans NVS and resets provisioning.

`tools/layout_check.py` models the 320x240 screen geometry and reports overlapping widgets before you flash; it exits non-zero if anything collides.

`python tools/run_host_tests.py --cc gcc` tests the production CoAP/REST parsers,
Wi-Fi failure paths and settings logic on a host. Use `--cc path/to/zig.exe` on
Windows if Zig is your host compiler. These tests and the layout check also run
in CI; they do not replace device commissioning, reconnect or camera tests.
