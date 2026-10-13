# Battery improvements — 15 September 2026

## Connected-device baseline

The CoreS3 on COM5 reported 100%, charge complete, VBAT 4117 mV,
VBUS 4841 mV, VSYS 4318 mV, PMIC 42 C and ESP32 44 C. Its saved
brightness was 95% and its screen timeout five minutes. These are device
readings, not independent voltage or capacity measurements. A charge-complete
flag is consistent with a full battery; it does not calibrate the gauge.

The running firmware identified itself as `888f04a*`, ESP-IDF v5.5.4. A private
16 MiB flash backup was saved outside the repository. Its application checksum
and validation hash pass, and its partition table matches the current project.

## Changes

- Enable CPU frequency scaling between 80 MHz idle and the configured maximum
  (160 MHz in this build). Keep APB at 80 MHz and automatic light sleep disabled
  to retain the H2 UART, USB console and border-router service.
- Dim the battery-powered screen to at most 20% after 30 idle seconds, with
  camera/sharing exceptions. Retain saved brightness and sleep timeout. Stop
  live label refreshes while asleep; continue power sampling and network work.
- Read both bytes of each voltage ADC result in one I2C transaction. Retry
  failed ADC setup without adding another device handle.
- Keep valid low-voltage zero gauge readings. Only use the coarse voltage
  fallback on battery, with plausible voltage bounds; mark it as an estimate.
  A zero gauge reading at high voltage can indicate gauge startup, but this
  heuristic is not a battery-specific calibration.
- Show unknown percentages as unknown, gate charge-complete on battery/USB
  presence, and label unplugged batteries as discharging.
- Reset discharge history on observed charging, missing/estimated readings,
  sampling gaps and upward gauge corrections. Require ten minutes and at least
  a two-point drop, reject stale results, and synchronize console/UI access.

The camera driver already disables sensor streaming and enables its analog
standby bit when stopped. Its warm-restart/power-rail sequence is unchanged.
The existing USB/battery Wi-Fi power-save policy is retained.

## Validation

Host tests pass for Wi-Fi (18 scenarios), settings, PMIC errors and source
selection, ADC burst reads/setup recovery, discharge continuity/quantisation/
staleness/wraparound, REST wire handling and MeshCoP codecs. The layout checker
reports zero problems across 18 panels plus the header.

Native ESP-IDF v5.5.4 build and the final incremental build pass. The application
is 2,763,280 bytes, leaving 62% of its partition free. Only the application at
`0x10000` was flashed; esptool verified the written image. Both main NVS and
Matter NVS compare byte-for-byte equal immediately before and after flashing.

The running `03882ab*` image's ELF SHA-256 matches the build:
`034ed581c3775a290a762410bc09b7fa4fa6049d6f836e1702f80c2a8af6cff5`.
Live checks confirm Wi-Fi associated, Matter still commissioned with its window
closed, and Thread attached as a child. Three GETs to `/node/state` on port 8081
returned the expected JSON state (53–95 ms). An initial probe used the wrong
port 80 and was refused; the configured 8081 endpoint passed.

The power console confirms 80–160 MHz with light sleep off, and correctly
reports external power instead of suggesting discharge measurement. Post-update
readings were 100%, charge complete, VBAT 4123 mV, PMIC 43 C and ESP32 45 C;
these do not demonstrate reduced consumption. Two successive camera starts
each captured 40 frames. Saved brightness remains 95%, timeout five minutes.
Camera capture was tested; QR interoperability and new Matter commissioning
were not tested. One stale Matter-session packet was discarded after reboot;
the retained commissioning state alone does not prove a new Matter transaction.

The user physically verified that unplugging dims the idle screen, touching
restores brightness, and reconnecting USB restores full configured brightness.
Twelve REST attachment checks over roughly one minute passed. After reconnecting,
uptime had advanced to 3m57s with zero Wi-Fi disconnects and Matter still
commissioned. The gauge then reported 99%, 4190 mV and constant-voltage charging;
the console correctly withheld the discharge estimate on USB.

## Measurement limits

No battery-only runtime, current draw, capacity, idle-frequency residency,
percentage-error curve or percentage improvement has been measured. A useful
follow-up is a timed unplugged run at a consistent workload, logging gauge and
voltage against time. Absolute gauge accuracy needs an independent capacity
reference; a voltage curve alone cannot provide it. The short unplugged screen
test does not establish battery runtime. Docker/CI is not part of this run.

The frequency policy follows [Espressif's ESP-IDF v5.5.4 power-management
documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/power_management.html).
Board power/peripheral context is documented by [M5Stack](https://docs.m5stack.com/en/core/CoreS3).

## Wi-Fi discharge recorder added later on 15 September

The firmware now provides read-only `/power/history` on port 8081. Its separate
8,640-sample PSRAM ring survives USB transitions and includes gauge/voltage,
measurement-validity flags, charge state, requested screen brightness and
monotonic timestamps. This history is separate from the short discharge-rate
window; charging still resets that estimator, but not the test log.

`tools/battery_record.py` drains the ring to CSV every 30 seconds, fsyncs each
saved sample, and reports collection health in a status JSON. Device boot IDs
and sequence numbers support restart deduplication and explicit loss detection.
The device ring is volatile; the PC CSV is the durable copy. Estimated UTC time
is anchored to PC receive time, with device uptime retained for calculations.

The application-only update is 2,766,784 bytes. The running ELF matches
`528c7d4053b31b12a86e72cfa63adad1f2e6508f41d3e03f8a889cd83c18b068`.
The prior application was backed up and checksum-validated before flashing;
both provisioning partitions remained byte-for-byte unchanged. Native build,
the full host suite, and 18-panel layout checks passed. Eight recorder tests
cover USB retention, PC restart, boot changes, loss detection, partial CSV
recovery, pagination/staleness, schema validation and a single-writer lock.
The real C ring is tested through capacity wraparound, allocation failure,
JSON serialization and cursor bounds.

Live HTTP pagination and malformed-query rejection pass. A real device reboot
preserved the prior CSV rows and collected the new boot starting at sequence 1,
without duplicates or sequence gaps. Camera capture still passed 40 frames.
The Windows recorder was launched hidden with a 24-hour deadline and an
automatic-sleep prevention request that ends when the process exits. It does
not change the Windows power plan or require this conversation to remain active.
At handoff, 39 live USB-powered samples were saved with no collection error or
stale-data flag. Physical unplugged capture with this new logger is still pending;
the recorder remains running and will collect it when USB is disconnected.

## Matter battery implementation, 15 September

The user ended the second discharge run and reconnected USB before requesting
implementation. Its archived CSV contains 707 battery samples spanning 117.76
minutes, from 100% to 76%. The earlier PC recorder interruption was recovered
from the device ring without missing sequence numbers. The recorder was stopped
and the completed CSV archived before deployment. This is an early discharge
baseline, not a full capacity or percentage calibration.

`main/matter_battery.cpp` appends a Power Source endpoint after the existing TBR
endpoint, with Battery and Rechargeable features. A scalar snapshot feeds the
CHIP event loop from the existing 10-second PMIC sample. Percentage uses Matter
half-percent units; invalid, absent, estimated or stale samples publish null.
Voltage is independently validated and rate-limited to one minute, with immediate
validity transitions. Charge state follows USB/charger flags. A bounded pending
job and 10-second watchdog retry failed scheduling/attribute updates and invalidate
input after 60 seconds without a fresh sample. Live readings do not write NVS.

The required non-nullable charge-level attribute uses warning below 15% and
critical at or below 5%; unknown gauge data retains its last known charge level,
while percentage and source status convey the missing measurement. Runtime and
current/capacity attributes are not exposed.

Local validation: full existing host regressions and the new production conversion
tests passed. A deterministic C++ harness also passed production publisher tests
for pre-start samples, burst coalescing, unchanged-value suppression, failed writes,
queue failures, voltage throttling, recovery and stale-sample invalidation. Layout
checks passed for 18 panels plus header. Native build and device/HA validation are
recorded below when completed.

The first native build passed, but live startup exposed the previous two-endpoint
limit (root plus TBR). The battery endpoint needs a third slot. The prior working
application was restored and its commissioned state verified while rebuilding
with a limit of three. A compile-time assertion now rejects an insufficient
endpoint count. Both provisioning partitions were byte-for-byte unchanged by
the initial app-only flash; the original application backup remains available.

Final deployment and live verification passed:

- Application size: 2,774,112 bytes; 62% of its partition remains free.
- Running ELF SHA256:
  `2509f3616e53ba548130496c242435e1c7963aa8220ef49f777988819097a20e`.
- Final app-only flash preserved both main NVS and Matter NVS byte-for-byte.
  The device booted commissioned with the commissioning window closed, Wi-Fi
  associated, and Thread attached as a child. Saved brightness (95%) and screen
  timeout (5 minutes) were retained. Camera self-test passed 40 frames.
- Home Assistant 2026.9.2 automatically discovered the new Power Source endpoint
  without removal, re-pairing, or manual re-interview. Its existing device retained
  the previous nine entities and gained three battery sensors:
  `sensor.cores3_thread_br_battery`, `sensor.cores3_thread_br_battery_voltage`,
  and `sensor.cores3_thread_br_battery_charge_state`.
- HA's interview contains endpoint 2, Power Source device type 0x0011, FeatureMap
  6 (Battery + Rechargeable), and EndpointList [0, 1, 2]. Root PartsList is [1, 2].
  Initial raw percentage 186 corresponds to the initial gauge sample of 93%; HA
  subsequently displayed 94%, matching a fresh gauge sample. Voltage was 4.162 V
  in HA and 4162 mV in the initial sample; charge state was charging.
- Live REST battery recording still returns valid USB-powered samples after the
  update. The completed discharge CSV remains archived; its PC recorder is stopped.

Physical unplug/replug and charge-completion transitions were not repeated after
this installation; those mappings and failure/stale cases passed host tests.
The live HA verification covered discovery and charging telemetry. Full battery
capacity/calibration and the additional reporting's runtime impact remain unmeasured.

## Battery display on wake, 17 September

The reported flash of "100% charging" followed by 80% was stale display content.
The archived device history shows 80%, USB absent and not charging both before
and after the touch, including samples collected with the screen off. The
10-second background sampler was working. Drawing was suspended during sleep,
but the wake handler previously restored the backlight before refreshing labels.

Wake now reads battery state, refreshes the header and any visible power page,
and renders before restoring the backlight. Failed or unknown reads clear the
header to `--` instead of retaining its previous percentage. A production-code
host harness reproduces the sleeping 100%-to-80% scenario and checks background
sampling, refresh-before-backlight ordering, hidden power pages and read failures.
The full host suite and native build passed.

App-only deployment preserved both NVS partitions byte-for-byte. The running ELF
matches `bafa2566d2aef6fbf37e5264e879f801dc282ffaf2562285809b1d02ebebf226`.
Matter commissioning, Wi-Fi, Thread attachment and saved display settings were
verified after reboot. Live device and HA readings both reported 84% charging
after USB reconnection; all three HA battery entities remained available.
Recent history and the previous application were archived before flashing.
Physical confirmation of the corrected wake display remains pending.
