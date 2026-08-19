# Building in CLion

This repo builds in Docker, so `build/compile_commands.json` points at paths
inside the container (`/project`, `/opt/esp/idf`). Those don't exist on
Windows, which is why CLion can't resolve `esp_log.h` or anything else from
ESP-IDF. For code insight and a working build button you need ESP-IDF
installed locally.

## 1. Install ESP-IDF v5.5.4

Get the Windows installer from Espressif and pick **v5.5.4**, the same version
the Docker image and CI use. A mismatched version will compile but can behave
differently on hardware. The default install path is `C:\Espressif`.

## 2. Clear the Docker build cache

`build/` was configured inside the container and its CMake cache still says
`CMAKE_HOME_DIRECTORY=/project`. CLion won't reuse it.

```
rmdir /s /q build cmake-build-debug
```

## 3. Give CLion the ESP-IDF environment

Easiest is to start CLion from the **ESP-IDF 5.5 PowerShell** shortcut the
installer creates, so `IDF_PATH` and the Xtensa toolchain are already set:

```
& "C:\Program Files\JetBrains\CLion 2026.2.0.1\bin\clion64.exe"
```

To avoid doing that every time, add a toolchain under **Settings > Build,
Execution, Deployment > Toolchains** and point its environment script at
`C:\Espressif\frameworks\esp-idf-v5.5.4\export.bat`, with the compilers set to
`xtensa-esp32s3-elf-gcc.exe` and `xtensa-esp32s3-elf-g++.exe`.

## 4. CMake profile

Under **Settings > Build, Execution, Deployment > CMake**, delete the default
profile and add one with:

| Field | Value |
|---|---|
| Build directory | `build` |
| CMake options | `-DIDF_TARGET=esp32s3` |
| Generator | Ninja |
| Toolchain | the ESP-IDF one |

Keep the build directory as `build`, or `idf.py flash` and CLion will disagree
about where the binaries are. Then reload. If it complains, use **Tools >
CMake > Reset Cache and Reload Project**.

## 5. Build and flash

Build the `app` target. To flash, run:

```
idf.py -p COM5 flash monitor
```

Serial is 115200. Opening the port resets the board, so the first thing you
see is a boot log.

## The Docker build still works

`build.ps1` is what CI uses and stays reproducible regardless of what is
installed locally. It compiles inside a Docker volume and drops the flashable
artifacts in `dist/`, so it no longer writes into `build/` and won't fight
CLion's CMake cache.

```
.\build.ps1 -Port COM5
```

Reference: [Espressif's CLion guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/third-party-tools/clion.html)
