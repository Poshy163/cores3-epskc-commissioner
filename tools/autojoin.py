"""Discover an active border agent and join it from the CoreS3, in one shot.

ePSKc keys are single-use and ot-br-posix picks a fresh UDP port on every
activation, so looking the port up and joining must happen together.

    python autojoin.py <passcode>
"""
import sys
import threading
import time

import serial
from zeroconf import ServiceBrowser, ServiceListener, Zeroconf

PORT = "COM5"
PASSCODE = sys.argv[1] if len(sys.argv) > 1 else "119377114"

found = {}
seen = threading.Event()


class L(ServiceListener):
    def add_service(self, zc, type_, name):
        info = zc.get_service_info(type_, name, timeout=4000)
        if not info:
            return
        v4 = [a for a in info.parsed_addresses() if ":" not in a]
        if not v4:
            return
        found["addr"], found["port"], found["name"] = v4[0], info.port, name
        seen.set()

    def update_service(self, zc, type_, name):
        self.add_service(zc, type_, name)

    def remove_service(self, zc, type_, name):
        pass


def main() -> int:
    zc = Zeroconf()
    br = ServiceBrowser(zc, "_meshcop-e._udp.local.", L())
    print("looking for an active ephemeral key...", flush=True)
    ok = seen.wait(20)
    br.cancel()
    zc.close()
    if not ok:
        print("no _meshcop-e advertised - start a key first:")
        print("  ot-ctl ba ephemeralkey start <9 digits> 600000")
        return 1

    addr, port = found["addr"], found["port"]
    print(f"found {found['name'].split('.')[0]} -> {addr}:{port}\n", flush=True)

    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = PORT, 115200, 0.3
    ser.dsrdtr = ser.rtscts = False
    ser.open()
    time.sleep(14)          # opening the port resets the S3
    ser.reset_input_buffer()

    cmd = f"join {addr} {port} {PASSCODE}"
    print(f"$ {cmd}", flush=True)
    ser.write((cmd + "\r\n").encode())
    ser.flush()

    chunks, deadline, last = [], time.time() + 90, time.time()
    while time.time() < deadline:
        d = ser.read(512)
        if d:
            chunks.append(d)
            last = time.time()
        elif time.time() - last > 15.0:
            break
    ser.close()

    out = b"".join(chunks).decode("utf-8", "replace")
    for ln in out.splitlines():
        if "wifi:" in ln or not ln.strip():
            continue
        print(ln.rstrip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
