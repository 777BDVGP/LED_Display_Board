// =============================================================================
// web_server.c — Shared HTTP Server Instance
// =============================================================================
// See web_server.h.
// =============================================================================

#include "esp_err.h"
#include "esp_log.h"

#include "web_server.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

// -----------------------------------------------------------------------------
// headOkHandler
// Answers every HTTP_HEAD request with a bare 200, regardless of path. Every
// page route below only registers HTTP_GET (unlike ESPAsyncWebServer on the
// ARD build, which answers HEAD on any matching GET route automatically), so
// without this the About page's post-OTA reload poll — a HEAD liveness probe
// against whatever page was open — always 404s and the page never reloads.
// -----------------------------------------------------------------------------
static esp_err_t headOkHandler(httpd_req_t *req)
{
    return httpd_resp_send(req, NULL, 0);
}

httpd_handle_t WebServer_Start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // CONFIG_LWIP_MAX_SOCKETS is 10 system-wide (shared with OTA's UDP socket,
    // DNS lookups, etc.) — httpd_start() failed outright when this was set to
    // 10, leaving it no headroom to reserve its own listening + client sockets.
    config.max_open_sockets  = 5;
    config.max_uri_handlers  = 64; // page/AJAX/CSV routes across all modules (55 used)
    config.lru_purge_enable  = true;
    config.stack_size        = 10240; // page handlers build/scan SPIFFS directories
    config.uri_match_fn      = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return NULL;
    }

    static const httpd_uri_t head_uri = { .uri = "/*", .method = HTTP_HEAD, .handler = headOkHandler };
    httpd_register_uri_handler(s_server, &head_uri);

    ESP_LOGI(TAG, "Web server started");
    return s_server;
}

httpd_handle_t WebServer_Handle(void)
{
    return s_server;
}
