#pragma once

#include <stdbool.h>
#include <stddef.h>

// Initialize WiFi in STA mode and connect. Returns true if connected.
bool wifi_init_sta(const char *ssid, const char *pass, int timeout_ms);

// Initialize WiFi in AP mode for setup
void wifi_init_ap(const char *ap_ssid);

// Get IP address string (caller provides buffer, min 16 bytes)
void wifi_get_ip_str(char *buf, size_t len);

// Disconnect and stop WiFi
void wifi_stop(void);
