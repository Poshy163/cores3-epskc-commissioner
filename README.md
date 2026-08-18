# CoreS3 ePSKc Commissioner

Firmware that turns an [M5Stack CoreS3 Thread BR](https://docs.m5stack.com/en/core/CoreS3_Thread_BR) into a Thread 1.4 border router that fetches its credentials from another border router over ePSKc. You type the 9-digit code on the touch screen, or scan it as a QR with the camera. The device runs the DTLS/EC-JPAKE handshake, pulls the Active Operational Dataset, joins the mesh, and shows up in Home Assistant's Thread panel next to your other border routers.

I built it to test the ePSKc side of Home Assistant:
[core #177371](https://github.com/home-assistant/core/pull/177371),
[python-otbr-api #267](https://github.com/home-assistant-libs/python-otbr-api/pull/267) and
[#269](https://github.com/home-assistant-libs/python-otbr-api/pull/269).
The cross-vendor path works: this ESP32 commissioner pulls a real dataset from a stock `ot-br-posix` border agent.

## Hardware

One M5Stack CoreS3 Thread BR (SKU K149). The ESP32-S3 runs this firmware. The on-board ESP32-H2 keeps its stock RCP firmware and never gets touched.

Flashing replaces M5Stack's stock border-router firmware. To go back, use M5Burner's factory restore.

## Build

Docker is the whole toolchain, nothing else to install:

```bash
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.5.4 idf.py build
```

On Windows Git Bash, prefix the command with `MSYS_NO_PATHCONV=1` or Docker mangles the `/project` path.

## Flash

```bash
cd build
python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash "@flash_args"
```

Swap `COM5` for your port. `pip install esptool` if you don't have it.

## Wi-Fi

Two options:

* Over the console (recommended): plug in USB, open a 115200 serial terminal, and run `wifi "My Network" mypassword`. Saved to flash, survives reflashes.
* At build time: `idf.py menuconfig` → "ePSKc Commissioner" → set SSID and password. Only used when the device has nothing stored. The password lands in the firmware image in plain text, so keep it out of anything you commit or share.

## Using it

1. Ask your border router for an ephemeral key. In Home Assistant that's the Thread panel once the linked PRs land; today you can run `ot-ctl ba ephemeralkey start <9 digits> 300000` inside the OTBR add-on container.
2. On the device: **Scan for routers**, pick yours from the list.
3. Type the code, or press **QR** and hold the code up to the camera.
4. Watch it join. Keys are single use and expire, so if the join fails, mint a fresh one.

## Console commands

| Command | Does |
|---|---|
| `wifi <ssid> <pass>` | connect and persist |
| `status` | Wi-Fi and IP state |
| `join <ip> <port> <code>` | run the ePSKc exchange from the console |
| `thread` | role and stored credentials |
| `forget` | erase Thread credentials |
| `batt` | battery %, voltage, charge state |
| `name <hostname>` | set the mDNS name other tools see |
| `camtest` | grab frames, print stats and ASCII art |
| `reveal 1` | unmask the network key in output. It prints secrets, so leave it off |

## Known issues

* A heap corruption in the camera stack crashes the serial console after `camtest` runs. The touch UI and border router keep working. Being chased.
* QR decode is unverified end to end. The sensor's init table ships with horizontal mirror on, which made decoding impossible; the firmware now clears it.
* The device attaches as `child`, which is normal on a mesh that already has plenty of routers. Border-router duties don't depend on the radio role.

## tools/

The PC-side pieces used while building this: a standalone C commissioner PoC (`epskc_commissioner.c` plus a Dockerfile that builds mbedTLS with EC-JPAKE enabled, which distro packages ship without), `autojoin.py` for one-shot discover-and-join over serial, and `robust_cmd.py` for console commands that survive USB re-enumeration.

## Web flashing

Anyone with Chrome or Edge can flash straight from the browser, no installs:

```
https://espressif.github.io/esp-launchpad/?flashConfigURL=https://raw.githubusercontent.com/Poshy163/cores3-epskc-commissioner/main/launchpad.toml
```

[ESP Launchpad](https://espressif.github.io/esp-launchpad/) reads `launchpad.toml` and flashes the merged binary from `firmware/`. The GitHub Actions workflow rebuilds that binary on every push, so the link always serves the latest code.

A browser-flashed device has no Wi-Fi credentials baked in. Provision it over the console: Launchpad has a built-in serial monitor (or use any 115200 terminal) and run:

```
wifi "My Network" mypassword
```

Saved to flash, survives reboots and reflashes.
