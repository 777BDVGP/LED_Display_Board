// =============================================================================
// sensors.h — Button / Radar / LDR Sensor Reads (ESP-IDF port)
// =============================================================================
// Ported from LED_Display_Board_ARD's 08_APPEvents.cpp sensor helpers.
// =============================================================================

#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>
#include <stddef.h>

// Sets up the ADC1 unit + LDR channel (GPIO35) and the RADAR_SENSE rising-edge
// interrupt. Call once, after SetupSystemIOs(), before any other Sensors_* call.
void Sensors_Init(void);

bool Sensors_GetButtonTerminate(void);   // TERM_BUTTON, active HIGH
bool Sensors_GetRadarSense(void);        // RADAR_SENSE, active HIGH

// True if the radar is HIGH now OR has had a rising edge since the last call
// (latched by a GPIO interrupt, so short pulses between polls aren't lost).
// Clears the latch.
bool Sensors_TakeRadarTrigger(void);

// Formats "RRRR:BBB" (10-sample-averaged raw LDR : computed brightness) for
// the on-screen LDR debug view.
void Sensors_GetLDRDebugText(char *out, size_t outsz);

// Computed display brightness (clamped to S_DISP_BRIGHT_MIN..MAX), gated by
// S_DISPLAY_ON_HUMAN_DET + the TIM_HUMAN_DETECT_STAY_ON timer when enabled.
int Sensors_GetBrightness(void);

// (Re)sets TIM_HUMAN_DETECT_STAY_ON's duration from S_DAY_DETECT_OFF_MIN or
// S_NIGHT_DETECT_OFF_SEC — whichever window the current time falls in, per
// S_DAY_DETECT_OFF_START / S_NIGHT_DETECT_OFF_START (same wrap-around logic
// as GetTimeDisplayColor) — then activates it. Call this instead of
// Timers_Activate(TIM_HUMAN_DETECT_STAY_ON) directly, since the correct
// duration can only be known at the moment of activation, not at boot.
void HumanDetect_ActivateStayOn(void);

#endif // SENSORS_H
