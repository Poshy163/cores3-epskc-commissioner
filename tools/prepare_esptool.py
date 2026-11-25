"""Print a project-local PYTHONPATH for the pinned firmware image tool."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import sysconfig
from tempfile import TemporaryDirectory


def check_version(directory, version):
    env = os.environ.copy()
    paths = [str(directory), env.get("PYTHONPATH", "")]
    env["PYTHONPATH"] = os.pathsep.join(filter(None, paths))
    # Import the same module and CLI dependencies as IDF's python -m esptool.
    subprocess.run(
        [sys.executable, "-c",
         "import esptool, sys; sys.exit(0 if esptool.__version__ == sys.argv[1] else 1)",
         version], env=env, check=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", type=Path, required=True, help="project cache directory")
    parser.add_argument("--standalone", action="store_true",
                        help="install runtime dependencies for Python outside ESP-IDF")
    args = parser.parse_args()
    requirements = Path(__file__).with_name("requirements-esptool.txt")
    version = requirements.read_text().strip().removeprefix("esptool==")
    runtime = f"{sys.implementation.cache_tag}-{sysconfig.get_platform()}"
    mode = "standalone" if args.standalone else "idf"
    cache = args.target.resolve() / runtime
    destination = cache / f"esptool-{version}-{mode}"
    if not destination.is_dir():
        cache.mkdir(parents=True, exist_ok=True)
        with TemporaryDirectory(dir=cache) as temp:
            staged = Path(temp) / "packages"
            command = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check",
                       "--target", str(staged), "-r", str(requirements)]
            if not args.standalone:
                # ESP-IDF supplies the Python dependencies for its esptool.
                # Keep its cryptography, click and other SDK packages in use.
                command.append("--no-deps")
            subprocess.run(command, stdout=sys.stderr, check=True)
            check_version(staged, version)
            try:
                staged.rename(destination)
            except OSError:
                if not destination.is_dir():  # a concurrent build can win the rename
                    raise
    check_version(destination, version)
    print(destination)


if __name__ == "__main__":
    main()
