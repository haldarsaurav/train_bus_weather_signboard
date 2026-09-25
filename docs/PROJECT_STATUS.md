# Rev1.1 project closeout

**Software, documentation and repository packaging completed on 25 September 2026.**
Firmware remains **1.1.0**. The `rev1.1-final` tag adds the final documentation and permission
policy to the reviewed firmware revision; the earlier `rev1.1` tag is preserved unchanged.

## Delivered

- Reviewed firmware, including corrected delay snapshots and the prior reliability fixes.
- A dedicated [code repository](https://github.com/haldarsaurav/train_bus_weather_signboard_code)
  for firmware, tests and build/release documentation.
- A [public showcase](https://github.com/haldarsaurav/train_bus_weather_signboard) whose current
  branch contains documentation and images, with no firmware source or compiled firmware.
- The [complete feature guide](https://github.com/haldarsaurav/train_bus_weather_signboard/blob/main/docs/FEATURE_GUIDE.md),
  covering screen fields, symbols, colours, graph legends, controls, examples and limitations.
- Two enlarged explanation diagrams, visually checked for clipping and legibility.
- Matching proprietary [licence](../LICENSE), [AI-use policy](../AI_POLICY.md) and
  [permission guide](PERMISSIONS.md) in both repositories.
- A complete local Rev1.1 ZIP containing reviewed source, documentation and existing CAD
  snapshots, with a per-file SHA-256 manifest. Private settings and reference photos are excluded.

## Verification

The firmware release passed **52 compile-time logic/graph assertions**, **five actual embedded
setup JavaScript scenarios**, and ESP32-C3 compile/link/image generation. Finalization changes
only documentation, notices and illustration assets; the executable source matches the tested
Rev1.1 source. Documentation links, image files, repository separation and archive checksums
are checked as part of final packaging.

## Physical validation still open

The board was not flashed in this review. Physical display appearance, tiny marker legibility,
MODE and setup interactions on the device, Wi-Fi outage recovery over a long run, and enclosure
fit need an actual hardware session. Software/documentation completion does not certify those
physical results. The illustrations are not evidence of hardware testing.

## Publication and rights scope

The public repository's older commits may still contain the original firmware. They were not
rewritten. The new notices do not retrospectively revoke earlier valid grants or GitHub platform
rights, and cannot physically prevent copying or AI access. The owner nevertheless expressly
requires prior written permission for the protected reuse described in the licence.
