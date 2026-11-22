// =============================================================================
// system_io.h — Hardware I/O Setup
// =============================================================================
// ESP-IDF port of 05_ThisSystem.cpp's SetupSystemIOs().
// =============================================================================

#ifndef SYSTEM_IO_H
#define SYSTEM_IO_H

// Configures all application-level GPIO pins. Called once from app_main()
// before WiFi connection is attempted so pins are in a defined state at boot.
void SetupSystemIOs(void);

#endif // SYSTEM_IO_H
