#pragma once

#include <stdbool.h>

// Start a captive portal DNS server that redirects all lookups to the given IP
void dns_server_start(const char *redirect_ip);

// Stop the DNS server
void dns_server_stop(void);
