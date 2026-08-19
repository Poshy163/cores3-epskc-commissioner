# Building in CLion

`CMakeLists.txt` starts with:

```cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

Without `IDF_PATH` that reads `include(/tools/cmake/project.cmake)`, CMake
aborts before defining a target, and CLion reports "this file does not belong
to any project" for everything under `main/`. If it configures but you still
get `Cannot resolve symbol 'uint8_t'`, CLion is configuring with its default
toolchain instead of the Xtensa one, so none of the target's standard headers
are on the include path.

Both are fixed by a local ESP-IDF plus the checked-in CMake preset.

## 1. Install ESP-IDF v5.5.4

Already installed on this machine at `C:\Espressif\frameworks\esp-idf-v5.5.4`,
with the tools under `%USERPROFILE%\.espressif`. To reproduce elsewhere:

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

## 2. Environment

`IDF_PATH`, `IDF_PYTHON_ENV_PATH`, `IDF_TOOLS_PATH` and the tool directories are
already set as persistent user variables, so CLion picks them up wherever it is
launched from. Nothing to do unless you move the installation.

If you ever need to redo it, source the export script and copy what it sets:

```powershell
. C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
```

Environment changes only reach newly started processes, so restart CLion (fully,
not just the project) after any change here.

## 3. Open the project

The repo ships `CMakePresets.json` with an **esp32s3** preset that mirrors what
`idf.py` configures: Ninja, `build/` as the binary directory, `IDF_TARGET`, and
ESP-IDF's `toolchain-esp32s3.cmake`. CLion picks presets up automatically.

Open the project, choose the **ESP32-S3 (ESP-IDF)** profile, and let it reload.
Because the preset points at the same `build/` the CLI uses, CLion and `idf.py`
share one cache instead of fighting over two.

If a stale `build/` or `cmake-build-debug/` from an earlier attempt is present,
delete it first. A cache written inside the container records
`CMAKE_HOME_DIRECTORY=/project` and CLion will not reuse it.

## 4. Build and flash

Build the `app` target, or from a terminal:

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
