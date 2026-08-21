# CoreS3 ePSKc Commissioner

Firmware that turns an [M5Stack CoreS3 Thread BR](https://docs.m5stack.com/en/core/CoreS3_Thread_BR) into a Thread 1.4 border router you can drive from the touch screen. It does both halves of ePSKc: it can **fetch** credentials from another border router, and **hand its own out** to a commissioner. It also serves the OpenThread Border Router REST API, so Home Assistant can talk to it like any other OTBR.

## Web flashing

Anyone with Chrome or Edge can flash straight from the browser, no installs:

```
https://espressif.github.io/esp-launchpad/?flashConfigURL=https://raw.githubusercontent.com/Poshy163/cores3-epskc-commissioner/main/launchpad.toml
```

[ESP Launchpad](https://espressif.github.io/esp-launchpad/) reads `launchpad.toml` and flashes the merged binary from `firmware/`. The GitHub Actions workflow rebuilds that binary on every push, so the link always serves the latest code.

## Hardware

One M5Stack CoreS3 Thread BR (SKU K149). The ESP32-S3 runs this firmware. The on-board ESP32-H2 keeps its stock RCP firmware and never gets touched.

Flashing replaces M5Stack's stock border-router firmware. To go back, use M5Burner's factory restore.

## Wi-Fi

Two ways to get the device online:

* On the screen: **Settings > Wi-Fi**, pick your network from the scan, type the password on the keyboard. Saved to flash, survives reboots and reflashes. **Leave** disconnects and erases the credentials.
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

**Network** shows the details, how many devices have joined, and three actions:

* **Share** puts a 9-digit ePSKc code on screen and advertises `_meshcop-e._udp`, so a commissioner can pull the credentials from this device.
* **QR** renders the Active Dataset TLVs, the form Home Assistant accepts for "add network".
* **Forget** erases the stored credentials.

Anything destructive needs a second tap to confirm.

## REST API for Home Assistant

The device serves the OTBR REST API on port 8081, so you can add it in Home Assistant as an OpenThread Border Router pointed at `http://<device-ip>:8081`.

| Endpoint | Methods |
|---|---|
| `/node` | GET |
| `/node/ba-id`, `/node/ext-address`, `/node/ext-panid`, `/node/network-name`, `/node/rloc16` | GET |
| `/node/state` | GET, POST (`enable` / `disable`) |
| `/node/dataset/active` | GET (hex TLVs with `Accept: text/plain`), PUT |
| `/node/dataset/pending` | GET |
| `/node/ba-epskc/state` | GET, PUT |
| `/node/ba-epskc/key` | GET, POST, DELETE |

The `ba-epskc` endpoints follow [python-otbr-api #267](https://github.com/home-assistant-libs/python-otbr-api/pull/267). They can be switched off at **Settings > Thread > ePSKc over REST**, which *unregisters* them rather than returning an error, so the API becomes indistinguishable from a build without ePSKc support. That makes it easy to test both branches of a client.

Plain HTTP with no authentication, exactly like ot-br-posix: anyone who can reach port 8081 can read the Active Operational Dataset, which contains the network key. Keep it on a trusted network.

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
| `thread` | role, credentials, parent signal, border-router boot status |
| `forget` | erase Thread credentials |
| `rest [start\|stop\|epskc on\|off]` | OTBR REST API |
| `power` | voltages, temperatures, discharge rate |
| `settings` | show persisted settings |
| `batt` | battery %, voltage, charge state |
| `name <hostname>` | set the mDNS name |
| `camtest` | grab frames, print stats and ASCII art |
| `reveal 1` | unmask the network key in output. It prints secrets, so leave it off |

## Building

See [CLION.md](CLION.md) for a local ESP-IDF setup. `build.ps1` reproduces the CI build in Docker.

`tools/layout_check.py` models the 320x240 screen geometry and reports overlapping widgets before you flash; it exits non-zero if anything collides.
