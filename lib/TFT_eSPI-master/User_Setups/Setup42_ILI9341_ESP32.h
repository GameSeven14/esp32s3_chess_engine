// See SetupX_Template.h for all options available
#define USER_SETUP_ID 42

#define ST7789_DRIVER

#define TFT_MISO 42
#define TFT_MOSI 47
#define TFT_SCLK 21
#define TFT_CS 41  // Chip select control pin
#define TFT_DC 40  // Data Command control pin
#define TFT_RST 45 // Reset pin (could connect to RST pin)

// Optional touch screen chip select
#define TOUCH_CS 39 // Chip select pin (T_CS) of touch screen

#define LOAD_GLCD // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in
                  // FLASH
// Unused fonts disabled to save Flash and RAM (SPIFFS/VFS):
// #define LOAD_FONT2
// #define LOAD_FONT4
// #define LOAD_FONT6
// #define LOAD_FONT7
// #define LOAD_FONT8
// #define LOAD_GFXFF
// #define SMOOTH_FONT

// TFT SPI clock frequency
// #define SPI_FREQUENCY  20000000
// #define SPI_FREQUENCY  27000000
#define SPI_FREQUENCY 40000000
// #define SPI_FREQUENCY 80000000

// Optional reduced SPI frequency for reading TFT
#define SPI_READ_FREQUENCY 16000000

// SPI clock frequency for touch controller
#define SPI_TOUCH_FREQUENCY 2500000
