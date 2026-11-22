// =============================================================================
// font5x7.h — 5x7 Bitmap Font (printable ASCII 0x20-0x7E)
// =============================================================================
// Byte-for-byte the same glyph table as Adafruit_GFX's classic glcdfont.c
// (extracted programmatically from LED_Display_Board_ARD's own library
// cache), so text rendered here looks identical to the Arduino build.
// Characters outside 0x20-0x7E render as blank.
// =============================================================================

#ifndef FONT5X7_H
#define FONT5X7_H

#include <stdint.h>

// Renders text into the matrix framebuffer (see led_matrix.h) with its
// top-left glyph corner at (x0,y0). Glyphs are 5px wide + 1px spacing (6px
// per character); x0 may be negative or run past MTX_WIDTH for scrolling —
// matrix_set_pixel() clips out-of-range pixels.
void font_draw_text(const char *text, int x0, int y0, uint8_t r, uint8_t g, uint8_t b);

// Total pixel width text would occupy if fully drawn (chars*6 - 1, 0 if empty).
int font_text_width(const char *text);

#endif // FONT5X7_H
