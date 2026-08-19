# Building in CLion

This repo builds in Docker, so `build/compile_commands.json` points at paths
inside the container (`/project`, `/opt/esp/idf`). Those don't exist on
Windows, so no ESP-IDF header resolves and CLion reports "this file does not
belong to any project" for everything under `main/`.

The real cause is `CMakeLists.txt`:

```cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

With `IDF_PATH` unset that reads `include(/tools/cmake/project.cmake)`, CMake
aborts before defining a target, and nothing gets indexed. A local ESP-IDF is
the fix.

## 1. Install ESP-IDF v5.5.4

Already installed on this machine at `C:\Espressif\frameworks\esp-idf-v5.5.4`,
with the toolchain under `%USERPROFILE%\.espressif`. To reproduce elsewhere:

```powershell
git clone -b v5.5.4 --depth 1 --recursive --shallow-submodules `
    https://github.com/espressif/esp-idf.git C:\Espressif\frameworks\esp-idf-v5.5.4
cd C:\Espressif\frameworks\esp-idf-v5.5.4
.\install.ps1 esp32s3
```

Run that from PowerShell, not Git Bash. `idf_tools.py` detects MSYS and refuses
with "MSys/Mingw is not supported".

Match v5.5.4 to the Docker image and CI. A different version compiles but can
behave differently on hardware.

## 2. Give CLion the ESP-IDF environment

Source the export script, then launch CLion from that same shell so it inherits
`IDF_PATH` and the Xtensa toolchain:

```powershell
. C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
& "C:\Program Files\JetBrains\CLion 2026.2.0.1\bin\clion64.exe"
```

To skip that each time, add a toolchain under **Settings > Build, Execution,
Deployment > Toolchains** with:

| Field | Value |
|---|---|
| Environment script | `C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat` |
| C compiler | `%USERPROFILE%\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe` |
| C++ compiler | the matching `xtensa-esp32s3-elf-g++.exe` |
| Debugger | `%USERPROFILE%\.espressif\tools\xtensa-esp-elf-gdb\...\xtensa-esp32s3-elf-gdb.exe` |

## 3. CMake profile

Under **Settings > Build, Execution, Deployment > CMake**, delete the default
profile and add one with:

| Field | Value |
|---|---|
| Build directory | `build` |
| CMake options | `-DIDF_TARGET=esp32s3` |
| Generator | Ninja |
| Toolchain | the ESP-IDF one |

Keep the build directory as `build`, or `idf.py flash` and CLion disagree about
where the binaries are. Reload; if it complains, use **Tools > CMake > Reset
Cache and Reload Project**.

If a stale `build/` or `cmake-build-debug/` from an earlier attempt is present,
delete it first. A cache written inside the container records
`CMAKE_HOME_DIRECTORY=/project` and CLion will not reuse it.

## 4. Build and flash

Build the `app` target, then:

```
idf.py -p COM5 flash monitor
```

Serial is 115200. Opening the port resets the board, so the first thing you see
is a boot log.

## The Docker build still works

`build.ps1` is what CI uses and stays reproducible regardless of what is
installed locally. It compiles inside a Docker volume and drops the flashable
artifacts in `dist/`, so it never writes into `build/` and won't fight CLion's
CMake cache.

```
.\build.ps1 -Port COM5
```

Reference: [Espressif's CLion guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/third-party-tools/clion.html)
