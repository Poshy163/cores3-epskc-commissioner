"""Send a console command and collect output, surviving USB re-enumeration.

USB-Serial-JTAG drops the handle when the device is busy (heavy DMA from the
camera does it reliably), so reconnect and keep reading rather than aborting.
"""
import sys
import time

import serial

PORT = sys.argv[1]
CMD = sys.argv[2]
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0


def open_port():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, 115200, 0.2
    s.dsrdtr = s.rtscts = False
    # USB-Serial/JTAG maps these lines to reset/boot.  Set their inactive
    # levels before opening so repeated smoke-test commands do not reboot the
    # device and invalidate continuity checks.
    s.dtr = False
    s.rts = False
    s.open()
    return s


def main() -> int:
    chunks = []
    deadline = time.time() + SECONDS
    sent = False
    ser = None

    while time.time() < deadline:
        try:
            if ser is None:
                ser = open_port()
                time.sleep(1.0)
                if not sent:
                    ser.reset_input_buffer()
                    ser.write((CMD + "\r\n").encode())
                    ser.flush()
                    sent = True
            data = ser.read(512)
            if data:
                chunks.append(data)
        except Exception:
            try:
                if ser:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(0.6)

    try:
        if ser:
            ser.close()
    except Exception:
        pass

    out = b"".join(chunks).decode("utf-8", "replace")
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
