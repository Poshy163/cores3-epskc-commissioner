"""Browse for Thread border agent mDNS services.

_meshcop._udp   = the always-on border agent
_meshcop-e._udp = published only while an ephemeral key is active
"""
import sys
import time

from zeroconf import ServiceBrowser, ServiceListener, Zeroconf

# Windows consoles default to cp1252 and blow up on non-Latin-1 TXT values.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")


def safe(s: str) -> str:
    return s.encode("ascii", "backslashreplace").decode("ascii")

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
TYPES = ["_meshcop._udp.local.", "_meshcop-e._udp.local."]


class Listener(ServiceListener):
    def _show(self, zc: Zeroconf, type_: str, name: str, tag: str) -> None:
        info = zc.get_service_info(type_, name, timeout=3000)
        stamp = time.strftime("%H:%M:%S")
        if not info:
            print(f"[{stamp}] {tag} {type_}  {safe(name)}  (no info)")
            return
        addrs = ", ".join(info.parsed_addresses())
        print(f"[{stamp}] {tag} {type_}")
        print(f"           name : {safe(name)}")
        print(f"           addr : {addrs}   port: {info.port}")
        for k, v in (info.properties or {}).items():
            key = k.decode("utf-8", "replace") if isinstance(k, bytes) else str(k)
            if isinstance(v, bytes):
                try:
                    val = v.decode("utf-8")
                    if not val.isprintable():
                        raise ValueError
                except (UnicodeDecodeError, ValueError):
                    val = v.hex()
            else:
                val = str(v)
            print(f"           txt  : {safe(key)} = {safe(val)}")

    def add_service(self, zc, type_, name):
        self._show(zc, type_, name, "+ ADD   ")

    def update_service(self, zc, type_, name):
        self._show(zc, type_, name, "~ UPDATE")

    def remove_service(self, zc, type_, name):
        print(f"[{time.strftime('%H:%M:%S')}] - REMOVE  {type_}  {safe(name)}")


def main() -> int:
    zc = Zeroconf()
    listener = Listener()
    browsers = [ServiceBrowser(zc, t, listener) for t in TYPES]
    print(f"watching {', '.join(TYPES)} for {DURATION:.0f}s ...\n")
    try:
        time.sleep(DURATION)
    finally:
        for b in browsers:
            b.cancel()
        zc.close()
    print("\ndone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
