#pragma once
// =====================================================================
//  DeskSecrets.example.h - template for your private settings.
//
//  Copy this file to DeskSecrets.h (same folder) and edit the copy.
//  DeskSecrets.h is git-ignored: it stays on your PC and is never
//  published. DeskDisplay.h includes it automatically when it exists.
// =====================================================================

// Setup access point password (WPA2, at least 8 characters).
#define AP_PASSWORD "change-me-please"

// Optional: join this 2.4 GHz network directly on first boot instead of
// opening the setup access point. Leave empty to use setup as normal.
#define WIFI_FALLBACK_SSID ""
#define WIFI_FALLBACK_PASS ""
