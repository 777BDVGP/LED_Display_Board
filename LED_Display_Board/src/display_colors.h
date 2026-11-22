// =============================================================================
// display_colors.h — RGB Parsing & Day/Night Colour Selection
// =============================================================================
// Ported from LED_Display_Board_ARD's 08_APPEvents.cpp (updateColorFrom,
// getTimeDisplayColor, hhmmToMinutes). Colours are passed around as plain
// r,g,b bytes rather than a packed RGB565 value — this port has no need for
// NeoMatrixBus's 16-bit Color() packing since led_matrix.h takes r,g,b directly.
// =============================================================================

#ifndef DISPLAY_COLORS_H
#define DISPLAY_COLORS_H

#include <stdint.h>

// Parses "R,G,B" into r,g,b (0-255 each, clamped). A component that can't be
// parsed keeps its ARD-matching default of 1.
void ParseRGBColor(const char *rgbString, uint8_t *r, uint8_t *g, uint8_t *b);

// "HH:MM" -> minutes since midnight (0..1439), -1 on error.
int HHMMToMinutes(const char *t);

// Day/night colour selection based on S_DAY_TIME_COLOR_START /
// S_NIGHT_TIME_COLOR_START (handles wrap-around, e.g. day starts 20:00).
// Falls back to white on any parse error.
void GetTimeDisplayColor(uint8_t *r, uint8_t *g, uint8_t *b);

#endif // DISPLAY_COLORS_H
