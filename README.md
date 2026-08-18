# CoreS3 ePSKc Commissioner

Firmware that turns an [M5Stack CoreS3 Thread BR](https://docs.m5stack.com/en/core/CoreS3_Thread_BR) into a Thread 1.4 border router that fetches its credentials from another border router over ePSKc. You type the 9-digit code on the touch screen, or scan it as a QR with the camera. The device runs the DTLS/EC-JPAKE handshake, pulls the Active Operational Dataset, joins the mesh, and shows up in Home Assistant's Thread panel next to your other border routers.

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

Three ways to get the device online:

* On the screen: tap **Wi-Fi**, pick your network from the scan, type the password on the keyboard. Saved to flash, survives reboots and reflashes. **Leave** disconnects and erases the credentials.
* Over the console: open a 115200 serial terminal (ESP Launchpad has one built in) and run `wifi "My Network" mypassword`.
* At build time: `idf.py menuconfig` → "ePSKc Commissioner" → set SSID and password. Only used when the device has nothing stored. The password lands in the firmware image in plain text, so keep it out of anything you commit or share.

## Using it

1. Ask your border router for an ephemeral key. In Home Assistant that's the Thread panel once ePSKc support lands; today you can run `ot-ctl ba ephemeralkey start <9 digits> 300000` inside the OTBR add-on container.
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
