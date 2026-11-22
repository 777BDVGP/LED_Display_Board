// =============================================================================
// led_matrix.h — RMT-Driven WS2812 Matrix (ESP-IDF port of NeoMatrixBus)
// =============================================================================
// Drives the same physical panel arrangement as LED_Display_Board_ARD's
// NeoMatrixBus (src/NeoMatrixBus.h): 6 chained 8x8 WS2812 panels, laid out
// left-to-right, each panel scanned top+left+rows+progressive (no snake).
//
// Unlike the Arduino build (Adafruit_NeoPixel — bit-banged / RMT via the
// Arduino core, reported to occasionally corrupt frames when WebSocketTask's
// WiFi traffic runs on the other core), this uses ESP-IDF's native RMT TX
// driver directly: the bit timing is generated entirely in hardware from a
// pre-encoded symbol buffer with its own DMA-fed interrupt, so it is not
// sensitive to scheduling jitter from WiFi or any other task.
// =============================================================================

#ifndef LED_MATRIX_H
#define LED_MATRIX_H

#include <stdint.h>

#define MTX_PANELS   6
#define MTX_PANEL_W  8
#define MTX_PANEL_H  8
#define MTX_WIDTH    (MTX_PANELS * MTX_PANEL_W)  // 48
#define MTX_HEIGHT   MTX_PANEL_H                  // 8

// Initialises the RMT TX channel + WS2812 encoder on the given GPIO and
// clears the framebuffer. Call once before any other matrix_* function.
void matrix_init(int gpio_num);

// Clears the framebuffer (does not push to the LEDs — call matrix_show()).
void matrix_clear(void);

// Sets one pixel in the framebuffer. Out-of-range (x,y) is silently ignored.
void matrix_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

// Global brightness scale applied at matrix_show() time (0-255), same
// behaviour as ARD's NeoMatrixBus::setBrightness().
void matrix_set_brightness(uint8_t brightness);

// Pushes the framebuffer to the physical LEDs over RMT. Blocks until the
// hardware transmission completes.
void matrix_show(void);

#endif // LED_MATRIX_H
