// =============================================================================
// web_valid_periods.h — Reminder Valid Periods web page
// =============================================================================
// GET  /validperiods                      list of periods
//      /validperiods?f_Create=1           blank editor
//      /validperiods?f_Edit=/ValidPeriod_1.json
//      /validperiods?f_DelList=1          delete list
//      /validperiods?f_DelFile=/ValidPeriod_1.json
// POST /vp_save  body: {"ref":"","name":"...","ranges":[{"F":"..","T":"..","Y":true}]}
// =============================================================================

#ifndef WEB_VALID_PERIODS_H
#define WEB_VALID_PERIODS_H

#include "esp_http_server.h"

void WebValidPeriods_RegisterRoutes(httpd_handle_t server);

#endif // WEB_VALID_PERIODS_H
