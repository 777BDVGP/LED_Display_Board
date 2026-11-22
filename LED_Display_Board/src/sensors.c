// =============================================================================
// sensors.c — Button / Radar / LDR Sensor Reads
// =============================================================================
// See sensors.h. Ported from LED_Display_Board_ARD's 08_APPEvents.cpp.
// =============================================================================

#include <stdatomic.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_adc/adc_oneshot.h"

#include "app_config.h"
#include "datetime.h"
#include "display_colors.h"
#include "sensors.h"
#include "settings_store.h"
#include "timer_engine.h"

#define LDR_ADC_UNIT     ADC_UNIT_1
#define LDR_ADC_CHANNEL  ADC_CHANNEL_7   // GPIO35

static adc_oneshot_unit_handle_t s_adcHandle = NULL;

// Set by the RADAR_SENSE rising-edge ISR, consumed by Sensors_TakeRadarTrigger().
// Catches radar pulses that start and end between two DisplayTask polls.
static atomic_bool s_radarLatched = false;

static void IRAM_ATTR radarIsr(void *arg)
{
    (void)arg;
    atomic_store(&s_radarLatched, true);
}

void Sensors_Init(void)
{
    adc_oneshot_unit_init_cfg_t initCfg = { .unit_id = LDR_ADC_UNIT };
    adc_oneshot_new_unit(&initCfg, &s_adcHandle);

    adc_oneshot_chan_cfg_t chanCfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(s_adcHandle, LDR_ADC_CHANNEL, &chanCfg);

    // Radar rising-edge latch. ESP_ERR_INVALID_STATE just means the shared
    // ISR service is already installed, which is fine.
    gpio_set_intr_type(RADAR_SENSE, GPIO_INTR_POSEDGE);
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_add(RADAR_SENSE, radarIsr, NULL);
    }
}

bool Sensors_GetButtonTerminate(void)
{
    return gpio_get_level(TERM_BUTTON) == 1;
}

bool Sensors_GetRadarSense(void)
{
    return gpio_get_level(RADAR_SENSE) == 1;
}

bool Sensors_TakeRadarTrigger(void)
{
    bool latched = atomic_exchange(&s_radarLatched, false);
    return latched || Sensors_GetRadarSense();
}

static int readRawLDR(void)
{
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        int raw = 0;
        if (s_adcHandle) adc_oneshot_read(s_adcHandle, LDR_ADC_CHANNEL, &raw);
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(2)); // space out readings, matches ARD's delay(2)
    }
    return sum / 10;
}

// Linear scale [ldrMin,ldrMax] -> [brightMin,brightMax]; 0 span falls back to brightMin.
static int scaleBrightness(int rawLDR, int ldrMin, int ldrMax, int brightMin, int brightMax)
{
    int span = ldrMax - ldrMin;
    if (span == 0) return brightMin;
    return brightMin + (rawLDR - ldrMin) * (brightMax - brightMin) / span;
}

void Sensors_GetLDRDebugText(char *out, size_t outsz)
{
    int rawLDR = readRawLDR();
    int ldrMin = Settings_GetNumber(S_LDR_RAW_MIN);
    int ldrMax = Settings_GetNumber(S_LDR_RAW_MAX);
    int brightMin = Settings_GetNumber(S_DISP_BRIGHT_MIN);
    int brightMax = Settings_GetNumber(S_DISP_BRIGHT_MAX);
    int brightness = scaleBrightness(rawLDR, ldrMin, ldrMax, brightMin, brightMax);
    snprintf(out, outsz, "%04d:%03d", rawLDR, brightness);
}

int Sensors_GetBrightness(void)
{
    int rawLDR = readRawLDR();
    int ldrMin = Settings_GetNumber(S_LDR_RAW_MIN);
    int ldrMax = Settings_GetNumber(S_LDR_RAW_MAX);
    int brightMin = Settings_GetNumber(S_DISP_BRIGHT_MIN);
    int brightMax = Settings_GetNumber(S_DISP_BRIGHT_MAX);

    if (rawLDR < ldrMin) rawLDR = ldrMin;
    if (rawLDR > ldrMax) rawLDR = ldrMax;

    int brightness = scaleBrightness(rawLDR, ldrMin, ldrMax, brightMin, brightMax);
    if (brightness < brightMin) brightness = brightMin;
    if (brightness > brightMax) brightness = brightMax;

    if (Settings_GetBool(S_DISPLAY_ON_HUMAN_DET)) {
        if (Sensors_GetRadarSense()) {
            HumanDetect_ActivateStayOn();
            return brightness;
        }
        return Timers_Expired(TIM_HUMAN_DETECT_STAY_ON) ? 0 : brightness;
    }
    return brightness;
}

void HumanDetect_ActivateStayOn(void)
{
    char nowStr[6];
    GetCurrentTime(nowStr, sizeof(nowStr));
    int nowMin = HHMMToMinutes(nowStr);

    char dayStartStr[6], nightStartStr[6];
    Settings_GetText(S_DAY_DETECT_OFF_START, dayStartStr, sizeof(dayStartStr));
    Settings_GetText(S_NIGHT_DETECT_OFF_START, nightStartStr, sizeof(nightStartStr));
    int dayStart   = HHMMToMinutes(dayStartStr);
    int nightStart = HHMMToMinutes(nightStartStr);

    bool isDay = true;
    if (nowMin >= 0 && dayStart >= 0 && nightStart >= 0) {
        isDay = (dayStart < nightStart)
            ? (nowMin >= dayStart && nowMin < nightStart)
            : (nowMin >= dayStart || nowMin < nightStart); // wrap-around
    }

    // Day window is in minutes; night window is in seconds (5 s minimum).
    if (isDay) {
        int minutes = Settings_GetNumber(S_DAY_DETECT_OFF_MIN);
        Timers_SetValue(TIM_HUMAN_DETECT_STAY_ON, (unsigned)minutes, MIN);
    } else {
        int seconds = Settings_GetNumber(S_NIGHT_DETECT_OFF_SEC);
        if (seconds < NIGHT_DETECT_OFF_MIN_SEC) seconds = NIGHT_DETECT_OFF_MIN_SEC;
        Timers_SetValue(TIM_HUMAN_DETECT_STAY_ON, (unsigned)seconds, SEC);
    }
    Timers_Activate(TIM_HUMAN_DETECT_STAY_ON);
}
