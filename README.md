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

The `ba-epskc` endpoints follow [python-otbr-api #267](https://github.com/home-assistant-libs/python-otbr-api/pull/267). They can be switched off at **Settings > Thread > ePSKc over REST**, which *unregisters* them rather than returning an error, so the API becomes indistinguishable from a build without ePSKc support. That makes it easy to test both branches of a client.

Plain HTTP with no authentication, exactly like ot-br-posix: anyone who can reach port 8081 can read the Active Operational Dataset, which contains the network key. Keep it on a trusted network.

## Matter and Home Assistant topology

The Matter node has a standard Root Node plus a Thread Border Router endpoint with Thread Network Diagnostics and Thread Border Router Management (including PAN Change). Matter and MeshCoP use the same saved device name, so the controller-facing TBR name matches the border-router service visible on the LAN.

This development build uses the Connectivity Standards Alliance test vendor/product range and Espressif's example device-attestation credentials. It is interoperable Matter firmware, but it is not a CSA-certified commercial product. Home Assistant Matter Server 9.0 and later therefore needs its **Test-Net DCL** option enabled before commissioning it.

On the serial console:

```text
matter status
matter code
matter open 900
```

Use the manual code or QR payload from `matter code` in Home Assistant's Matter add-device flow. An uncommissioned device advertises on the LAN and over BLE; after it has a fabric, `matter open` follows the Matter requirement to open subsequent commissioning on the operational network only. Once commissioned, Matter Server can interview endpoint `0x0091` and include the border router and its diagnostics in the Matter network topology.

## Settings

Wi-Fi, screen brightness and sleep timeout, a live power page, Thread options (router preference, fixed channel for new networks, share-code lifetime, REST toggles), device name, an About page with firmware hash and uptime, plus Reboot and Factory reset.

The power page reports battery percentage, voltages, PMIC and ESP32 temperatures, charge state, and a discharge rate estimated from the gauge. There is no live wattage: the AXP2101 has no current-sense channel.

## Console commands

| Command | Does |
|---|---|
| `wifi <ssid> <pass>` | connect and persist |
| `status` | Wi-Fi, IP and firmware version |
| `join <ip> <port> <code>` | run the ePSKc exchange from the console |
| `newnet` | form a new Thread network |
| `share [stop\|state]` | hand out an ePSKc code |
| `thread` | attachment, parent/direct children, local routers and traffic counters |
| `thread scan` | run a best-effort mesh topology scan and show devices seen |
| `forget` | erase Thread credentials |
| `rest [start\|stop\|epskc on\|off]` | OTBR REST API |
| `power` | voltages, temperatures, discharge rate |
| `settings` | show persisted settings |
| `batt` | battery %, voltage, charge state |
| `name <name>` | set the shared Matter/MeshCoP border-router name |
| `matter [status\|code\|open [seconds]]` | inspect or open Matter commissioning |
| `camtest` | grab frames, print stats and ASCII art |
| `reveal 1` | unmask the network key in output. It prints secrets, so leave it off |

## Building

See [CLION.md](CLION.md) for a local ESP-IDF setup. `build.ps1` uses the same ESP-IDF v5.5.4 release as CI and builds in Docker. Supplying `-Port` flashes the bootloader, partition table and app as separate ranges, so it does not overwrite NVS:

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
