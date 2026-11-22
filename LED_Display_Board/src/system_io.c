// =============================================================================
// system_io.c — Hardware I/O Setup
// =============================================================================
// Implements SetupSystemIOs(), which configures all application GPIO pins.
// Ported 1:1 from LED_Display_Board_ARD's 05_ThisSystem.cpp.
// =============================================================================

#include "system_io.h"
#include "app_config.h"
#include "driver/gpio.h"

// -----------------------------------------------------------------------------
// SetupSystemIOs
// Configures all application-level GPIO pins. Called once from app_main()
// before WiFi connection is attempted so pins are in a defined state at boot.
// -----------------------------------------------------------------------------
void SetupSystemIOs(void)
{
    gpio_reset_pin(LEDOUT);
    gpio_set_direction(LEDOUT, GPIO_MODE_OUTPUT);
    gpio_set_level(LEDOUT, 1);           // LED ON — power-on indicator

    gpio_reset_pin(TERM_BUTTON);
    gpio_set_direction(TERM_BUTTON, GPIO_MODE_INPUT);   // Terminate / cancel push-button

    gpio_reset_pin(RADAR_SENSE);
    gpio_set_direction(RADAR_SENSE, GPIO_MODE_INPUT);   // Radar presence-sensor output
}
