#pragma once

#include "esp_http_server.h"

// Start the web server. Pass ap_mode=true for setup/captive portal mode.
httpd_handle_t webserver_start(bool ap_mode);

// Stop the web server
void webserver_stop(httpd_handle_t server);
