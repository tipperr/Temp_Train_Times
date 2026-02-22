# Caltrain Departure Display

An ESP32 project that displays live Caltrain departure times and local weather on a CYD (Cheap Yellow Display) TFT screen.

## Features

- Shows the next two Caltrain departures (minutes until departure, service type, and departure time)
- Shows current temperature and precipitation status with a sun/umbrella icon
- Supports two locations — home (4th & King, SF) and Redwood City — switching automatically based on which WiFi network the device connects to at boot
- Screen is only on during a configured time window (default 8–9am) to preserve battery life
- Touch-to-wake outside the window for on-demand viewing

## Hardware

- ESP32 (CYD — ESP32-2432S028)
- ILI9341 TFT display (built in to CYD)
- XPT2046 touchscreen (built in to CYD)

## Dependencies

- [ArduinoJson](https://arduinojson.org/) `^7.0.4`
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) `^2.5.43`

## Setup

1. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your values:

```cpp
#define WIFI_SSID      "your-home-wifi-ssid"
#define WIFI_PASS      "your-home-wifi-password"
#define WIFI_SSID_RC   "your-redwood-city-wifi-ssid"
#define WIFI_PASS_RC   "your-redwood-city-wifi-password"
#define API_KEY_511    "your-511-api-key"
```

2. Build and flash with PlatformIO:

```bash
pio run --target upload
pio device monitor
```

## Data Source

Departure and weather data is fetched from a Cloudflare Worker that proxies the 511 SF Bay transit API and an open-meteo weather API. The worker returns both home and Redwood City data in a single request, with the device selecting the relevant fields based on its detected location.
