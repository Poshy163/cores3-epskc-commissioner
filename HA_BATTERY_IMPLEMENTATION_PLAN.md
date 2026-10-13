# Home Assistant battery entities implementation plan

Status: implemented, installed, and verified in Home Assistant on 15 September 2026 after the user ended the discharge test and reconnected USB. The original design below is retained for review; results and remaining physical-test limitations are recorded in BATTERY_AUDIT.md.

## Outcome

Expose the CoreS3 battery percentage on its existing Home Assistant Matter device. Include battery voltage and charge state as useful companion diagnostics, using the same PMIC samples that drive the screen and discharge log. The feature should operate independently of the PC recorder.

Proposed entities (Home Assistant chooses the actual entity IDs):

| Entity | Matter attribute | Behavior |
| --- | --- | --- |
| Battery | Power Source / BatPercentRemaining | 0–100% in HA; unknown when a trustworthy gauge reading is unavailable |
| Battery voltage | Power Source / BatVoltage | Cell voltage, published in millivolts |
| Battery charge state | Power Source / BatChargeState | Charging, not charging, full charge, or unknown |

The battery entity is the required deliverable. Voltage and charge state are small companion additions to the same cluster. Runtime estimates and calibration changes are deferred until the discharge test provides sufficient evidence.

## Findings and compatibility

- `main/matter.cpp` creates the Root Node and Thread Border Router endpoint, but no Power Source endpoint. The existing Matter node is the intended integration path.
- `main/ui.c:batt_timer_cb` reads power every 10 seconds and records it before the screen-asleep early return. This is the appropriate sample handoff point.
- `main/power.h` already provides percentage, cell voltage, battery presence, USB presence, charger state, and an estimated-percentage flag.
- The checked-in ESP-Matter dependency supports the Power Source device type (`0x0011`), Battery and Rechargeable features, nullable percentage, and voltage attributes. Its cluster validation requires exactly one of Wired or Battery; USB charging must not cause both feature bits to be set on this battery source.
- The current generated `sdkconfig` has `CONFIG_SUPPORT_POWER_SOURCE_CLUSTER` disabled. The implementation must enable it through the project's normal configuration defaults and confirm the regenerated build configuration.
- Current upstream [Home Assistant Matter sensor code](https://github.com/home-assistant/core/blob/dev/homeassistant/components/matter/sensor.py) discovers percentage, voltage, and charge-state entities from these attributes. It divides BatPercentRemaining by two. The installed HA and Matter Server versions and the device registry association have not been inspected in this planning task; those are live validation requirements, not confirmed compatibility results.

## Implementation sequence

1. **Add the standard data model.** Enable Power Source support and increase `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` from two to three in `sdkconfig.defaults` (the root consumes one slot). In `main/matter.cpp`, append a Power Source endpoint after the existing TBR endpoint, before Matter starts, preserving existing endpoint IDs. Configure Battery plus Rechargeable features, description, order, required battery metadata, and truthful source status. Explicitly create optional BatPercentRemaining and BatVoltage attributes; enabling Battery alone does not create them. Begin unknown readings as Matter null, never a synthetic 100%.

2. **Associate the source with the device.** Populate the source's EndpointList for the endpoints it powers using the pinned CHIP `PowerSourceServer::SetEndpointList` API; the server supplies this attribute through its attribute-access handler. Verify Descriptor/PartsList and any Power Source Configuration requirements against the pinned SDK data model. Check the source appears beneath the existing HA Matter device, with no duplicate physical device entry. Keep endpoint allocation stable across subsequent boots and updates.

3. **Feed existing samples into Matter.** Add a small C-callable handoff in `main/matter.h` and its implementation in `main/matter.cpp`, called from the existing UI battery callback before the sleep return. Copy only needed scalar fields and validity information. Coalesce samples into a bounded pending snapshot and schedule work on the CHIP event loop; do not perform I2C reads or block on Matter network work in the LVGL callback. Handle samples received before Matter startup and failed scheduling by retaining the latest snapshot for retry.

4. **Define reporting semantics.** Publish `2 * percent` for valid gauge values: 0% → 0, 50% → 100, 100% → 200. Keep genuine zero valid. Publish null for unreadable, absent, out-of-range, or voltage-estimated percentage; Matter's normal battery sensor cannot explain that a percentage is only a voltage estimate. Voltage remains independently publishable when the cell is present and its reading is valid. Use the existing USB-gated charging/full flags, not percentage alone, for charge state. Define required charge-level metadata conservatively and test its thresholds; do not invent battery capacity, replacement alarms, or current measurements.

5. **Limit reporting overhead.** Update percentage and charge state only when they change, with validity transitions handled immediately on the next sample. Publish voltage at most once a minute from the latest sample to avoid reporting ADC noise every 10 seconds. Use normal Matter attribute updates and subscriptions; do not add polling from HA, another PMIC sampling timer, or NVS writes for live readings. Initialize from a fresh sample on each boot. Mark readings unknown if samples stop arriving for a defined freshness window (proposed 60 seconds), using a lightweight check on the Matter work path. Retain failed updates for retry and keep the pending queue bounded during outages.

6. **Document discovery and operation.** Update README with the entities, percentage conversion, unknown/estimated policy, update cadence, and installed-version-specific re-interview procedure. Confirm the existing HA entry is actually the Matter device: the OTBR REST entry alone does not discover these attributes. Investigate interview/refresh first if an already-commissioned node does not expose the new endpoint; factory reset and removal/re-pairing are not routine migration steps.

## Validation after implementation is authorized

- Host tests for 0/1/50/99/100%, null and invalid input, estimated fallback, independent voltage validity, charging versus full versus unplugged, startup, stale samples, retry, and coalescing. Exercise production conversion/handoff logic rather than testing a duplicate mapping.
- Native ESP-IDF build with the enabled cluster; inspect endpoint attributes, feature conformance, required metadata, and image size. Run the relevant existing power, logger, and Matter regression checks.
- Finish and archive the current discharge run before device deployment. Back up the current app and provisioning partitions, app-flash only, and verify provisioning remains unchanged using the established workflow.
- In HA, confirm Battery belongs to the existing device and is a percentage battery sensor. Compare its value with a current valid device gauge reading, including the factor-of-two encoding, then verify updates while the screen is off.
- Exercise plug/unplug, charge completion, device reboot, HA subscription recovery, and missing/invalid readings. Use host fault injection for cases that cannot be produced safely on hardware. Verify the new endpoint is rediscovered without losing commissioning or existing entities.
- Confirm Wi-Fi, Thread routing, touchscreen behavior, and CSV logging remain functional. Measure reporting overhead with a later comparable discharge segment; do not claim improved runtime from adding telemetry.

## Completion criteria

The existing HA Matter device has a working Battery entity, receives changes without the PC recorder, and reports unknown rather than misleading readings on sensor failure. Existing provisioning and TBR behavior remain intact. Voltage and charge-state diagnostics work if included. A successful build alone is not completion: the actual HA entity and live subscription must be verified.

## Scope of this planning session

The original planning session added only this document. The subsequent implementation request authorized firmware work and deployment after the discharge run. Its completed CSV was archived before updating the device.
