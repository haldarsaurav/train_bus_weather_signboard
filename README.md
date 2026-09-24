# Train Bus Weather Signboard

**A desk-sized departure board for Freising: trains, local buses and weather on a 3.2-inch screen.**

An ESP32-C3 Super Mini brings the feel of a station sign to the desk. It connects to 2.4 GHz
Wi-Fi and displays public transport and weather feeds without a phone app or an API key.

**Current revision: Rev1.1 · firmware 1.1.0**

![The main screens](docs/images/hero.png)

This repository is the public feature showcase. Firmware, tests, build instructions and technical
release notes live exclusively in the [code repository](https://github.com/haldarsaurav/train_bus_weather_signboard_code).
Access to that repository depends on its permissions.

The screen pictures below are generated illustrations using example data from the earlier
layout. They are not photographs or Rev1.1 hardware captures; the older train footer in them
predates the revised delay graph described below.

## What it shows

| Feature | On the desk |
| --- | --- |
| Train departures | Freising station, up to 24 trains across four pages, six per page |
| Useful departure details | Line, destination, platform, departure time and minutes remaining |
| Reported delays | Planned time struck through beside the revised time; delay highlighted |
| Cancellations | Red struck-through destination and an X in the countdown column |
| Two bus boards | Bahnhof Stadtbus and P+R-Platz, up to ten services each |
| City-bus filtering | The Bahnhof board keeps town routes separate from regional/express coaches |
| Current weather | Temperature, feels-like, conditions, rain, humidity, wind and sunrise/sunset |
| Next twelve hours | Rain probabilities and a temperature trend |
| Three-day outlook | High/low, feels-like, UV, rain timing, sun times, daylight change, wind and moon phase |
| Temperature history | Last 30 daily highs plus today, coloured by temperature |
| Delay history | Three hours of reported train-board snapshots, with missing-data gaps |
| Day and night colours | Weather backgrounds follow daytime, twilight and night |

A timetable time without a delay is not proof that a train is on time. Realtime information
and cancellations depend on the operator data available through the feed.

## Train and bus pages

![Train page illustration](docs/images/screen_trains.png)

Long destinations can scroll when enabled. Platform/bay information appears when supplied.
The Bahnhof Stadtbus and P+R boards each have their own page and refresh status.

| Bahnhof Stadtbus | P+R-Platz |
| --- | --- |
| ![Stadtbus illustration](docs/images/screen_bus_stadtbus.png) | ![P+R illustration](docs/images/screen_bus_pr.png) |

## The improved delay graph

Rev1.1 shows the average positive reported delay of eligible realtime trains due within the
next hour. It uses the fetched board, across all train pages, and does not claim to measure
final per-train punctuality. The same upcoming train can contribute to successive snapshots.

- **Time:** 180 elapsed-minute columns, oldest left and newest right, with hourly guides.
- **Colour:** green below 1.5 minutes, amber below 4 minutes, red from 4 minutes.
- **Height:** 0–15 minutes; a white cap marks values above that display scale.
- **Red top markers:** cancellations, kept separate from the delay average.
- **Grey markers:** incomplete realtime coverage among services in the window.
- **Gaps:** no usable observation, an empty eligible set or a failed/missed refresh.

For example, one cancelled train and one reported on-time train show a cancellation marker
and a zero-delay reading. The cancellation is not converted into an invented number of minutes.
History starts blank after a reboot and fills while the device runs.

## Weather pages

| Now | Next three days |
| --- | --- |
| ![Weather now illustration](docs/images/screen_weather_now.png) | ![Three-day illustration](docs/images/screen_weather_3day.png) |

Choose a weather town in setup, or use approximate location detected from the internet
connection. The location affects weather; the configured transport stops remain in Freising.
Forecast dates and daylight calculations use Europe/Berlin time.

![Day, twilight and night colours](docs/images/sky_tints.png)

## Controls and setup

| Control | Action |
| --- | --- |
| Tap MODE | Next page |
| Hold MODE for one second | Show the hold-progress hint |
| Keep holding to three seconds | Open setup |
| Save in setup | Store settings and restart |
| RESET | Restart the device |

MODE is the board's BOOT button. Hold it after startup begins, not while powering on or pressing
RESET, because that can select the chip's download mode.

![Boot, hold hint and setup illustrations](docs/images/setup_flow.png)

On first use, join **Desk-Transport-Display** using the on-screen Wi-Fi QR. Once connected,
scan the settings QR or open **http://192.168.4.1/**. Setup can also be opened with MODE later.
It remains open until Save or RESET.

| Setting | Default / behaviour |
| --- | --- |
| Wi-Fi network | Type the name or choose a scanned network; hidden networks are supported |
| Wi-Fi password | Blank preserves the saved password when keeping the same network |
| Weather place | Automatic detection or a typed town |
| Automatic page changes | Off |
| Page interval | 10 seconds; selectable from 5 to 60 seconds |
| Scrolling long destinations | Off |

![Setup page illustration](docs/images/setup_page.png)

## Reliability in Rev1.1

Each transport feed and the weather feed has its own health state. A successful weather
refresh cannot hide a failed train refresh. Empty results, initial loading and failures are
distinguished. Countdown times continue advancing during outages and expired rows are removed.

Requests have bounded waits, MODE remains available during Wi-Fi joining, and setup saving
recovers cleanly from reported errors. Prolonged connectivity/data failures trigger recovery.
The device needs internet access for fresh information; retained readings can become stale.

Rev1.1 passed firmware compile/link checks, 52 logic/graph assertions and five setup-page
JavaScript tests. Physical flashing, the new graph's appearance and long-running hardware
stability still need verification.

## Hardware

- ESP32-C3 Super Mini
- 3.2-inch ILI9341 SPI TFT, 240 × 320 native, used as 320 × 240 landscape
- USB power and the existing BOOT/MODE button

See the [wiring reference](docs/SCHEMATIC.md). Board-specific backlight connections must match
the actual display module. Build and flashing instructions are maintained in the code repository.

## Revision history

**Rev1.1 — 24 September 2026:** corrected delay measurement and elapsed-time history; separate
cancellation and coverage markers; clearer missing-data behaviour. Includes the Rev1 review's
feed-health, countdown, setup, timestamp, network recovery and weather-date improvements.

**Rev1 — 24 September 2026:** local reviewed project package, firmware 1.0.1.

**Initial release:** train, bus and weather pages, setup portal, graphs and display animations.

## Data and project rights

Transport data is provided through [Transitous](https://transitous.org/), with timetable and
realtime contributions from its upstream sources. Forecast and historical weather come from
[Open-Meteo](https://open-meteo.com/). These services determine availability and coverage.

This is an independent personal project, not an official Deutsche Bahn or transit-operator product.
Copyright 2026 Sam (haldarsaurav). All rights reserved; see [LICENSE](LICENSE).
