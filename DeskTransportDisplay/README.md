# DeskTransportDisplay (Arduino sketch) — v1.0.0

Open **`DeskTransportDisplay.ino`** in the Arduino IDE; the other files open as tabs.
Everything else — wiring, board settings, features and troubleshooting — is in the
[repository README](../README.md).

| Tab | Role |
|-----|------|
| `DeskDisplay.h` | user options, panel config, constants, every shared declaration |
| `DeskSecrets.example.h` | copy to `DeskSecrets.h` (git-ignored) and put your passwords there |
| `DeskTransportDisplay.ino` | globals, boot splash, `setup()`, `loop()`, MODE button, restart log |
| `Util.ino` | text, time, sorting, formatting, page counts, button ISR |
| `Net.ino` | Wi-Fi join, HTTPS/JSON, Transitous, Open-Meteo, location, network task |
| `Draw.ino` | every screen, health dot, clock, graphs, page transition |
| `Portal.ino` | stored settings, setup access point, setup web page, setup screen |

**Controls:** tap MODE → next page · hold MODE 3 s → setup mode (http://192.168.4.1/) ·
Save in setup restarts · RESET restarts any time.

© 2026 Sam (haldarsaurav). All rights reserved — see [LICENSE](../LICENSE).
