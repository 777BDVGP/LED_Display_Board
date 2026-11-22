// =============================================================================
// led_matrix.c — RMT-Driven WS2812 Matrix
// =============================================================================
// See led_matrix.h. Pixel mapping mirrors ARD's NeoMatrixBus::drawPixel()
// exactly: panel = x / 8, col = x % 8, index = panel*64 + y*8 + col.
// =============================================================================

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#include "led_matrix.h"
#include "led_strip_encoder.h"

static const char *TAG = "led_matrix";

#define RMT_RESOLUTION_HZ 10000000  // 10 MHz, 1 tick = 0.1 us
#define MTX_LED_COUNT      (MTX_PANELS * MTX_PANEL_W * MTX_PANEL_H)  // 384

static rmt_channel_handle_t s_chan     = NULL;
static rmt_encoder_handle_t s_encoder  = NULL;
static uint8_t              s_fb[MTX_LED_COUNT][3];   // framebuffer, R,G,B per LED (strip-index order)
static uint8_t              s_txbuf[MTX_LED_COUNT * 3]; // WS2812 wire order (G,R,B) scratch buffer
static uint8_t              s_brightness = 255;

static inline int pixelIndex(int x, int y)
{
    int panel = x / MTX_PANEL_W;
    int col   = x % MTX_PANEL_W;
    return panel * (MTX_PANEL_W * MTX_PANEL_H) + y * MTX_PANEL_W + col;
}

void matrix_init(int gpio_num)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_chan));

    led_strip_encoder_config_t enc_cfg = { .resolution = RMT_RESOLUTION_HZ };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc_cfg, &s_encoder));

    ESP_ERROR_CHECK(rmt_enable(s_chan));

    matrix_clear();
    ESP_LOGI(TAG, "Matrix ready: %d panels, %dx%d, GPIO %d", MTX_PANELS, MTX_WIDTH, MTX_HEIGHT, gpio_num);
}

void matrix_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void matrix_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= MTX_WIDTH || y < 0 || y >= MTX_HEIGHT) return;
    int idx = pixelIndex(x, y);
    s_fb[idx][0] = r;
    s_fb[idx][1] = g;
    s_fb[idx][2] = b;
}

void matrix_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

void matrix_show(void)
{
    for (int i = 0; i < MTX_LED_COUNT; i++) {
        uint8_t r = (uint8_t)(((int)s_fb[i][0] * s_brightness) / 255);
        uint8_t g = (uint8_t)(((int)s_fb[i][1] * s_brightness) / 255);
        uint8_t b = (uint8_t)(((int)s_fb[i][2] * s_brightness) / 255);
        // WS2812 wire order is G,R,B (matches ARD's NeoMatrixBus NEO_GRB config).
        s_txbuf[i * 3 + 0] = g;
        s_txbuf[i * 3 + 1] = r;
        s_txbuf[i * 3 + 2] = b;
    }

    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(s_chan, s_encoder, s_txbuf, sizeof(s_txbuf), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_chan, portMAX_DELAY));
}
