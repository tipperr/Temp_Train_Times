#pragma once

// Display driver
#define ILI9341_DRIVER

// CYD TFT is wired to HSPI (not VSPI)
#define USE_HSPI_PORT

// SPI pins
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1

// Backlight (often GPIO21 on CYD; if yours differs, we can adjust)
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

// IMPORTANT: enable fonts (without these, text won’t render)
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

// Optional: if you later use smooth fonts from SPIFFS/LittleFS
// #define SMOOTH_FONT

#define SPI_FREQUENCY  40000000