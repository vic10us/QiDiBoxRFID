#include "dns_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"

static const char *TAG = "dns";
static int dns_sock = -1;
static TaskHandle_t dns_task_handle = NULL;
static uint32_t redirect_addr = 0;

// Minimal DNS response: answer all A queries with redirect_addr
static void handle_dns_request(uint8_t *buf, int len, struct sockaddr_in *client)
{
    if (len < 12) return;

    // Build response in-place
    buf[2] = 0x81; // QR=1, AA=1
    buf[3] = 0x80; // RA=1
    // Set answer count = 1
    buf[6] = 0x00;
    buf[7] = 0x01;

    // Skip question section to find its end
    int pos = 12;
    while (pos < len && buf[pos] != 0) {
        pos += buf[pos] + 1;
    }
    pos += 5; // null + qtype(2) + qclass(2)

    // Append answer: pointer to name in question, type A, class IN, TTL 60, 4 bytes
    uint8_t answer[] = {
        0xC0, 0x0C,             // pointer to name at offset 12
        0x00, 0x01,             // type A
        0x00, 0x01,             // class IN
        0x00, 0x00, 0x00, 0x3C, // TTL = 60s
        0x00, 0x04,             // data length = 4
        0, 0, 0, 0              // IP address (filled below)
    };
    memcpy(answer + 12, &redirect_addr, 4);

    if (pos + sizeof(answer) <= 512) {
        memcpy(buf + pos, answer, sizeof(answer));
        sendto(dns_sock, buf, pos + sizeof(answer), 0,
               (struct sockaddr *)client, sizeof(*client));
    }
}

static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len;

    while (dns_sock >= 0) {
        client_len = sizeof(client);
        int len = recvfrom(dns_sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len > 0) {
            handle_dns_request(buf, len, &client);
        }
    }
    vTaskDelete(NULL);
}

void dns_server_start(const char *redirect_ip)
{
    struct in_addr addr;
    inet_aton(redirect_ip, &addr);
    redirect_addr = addr.s_addr;

    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        return;
    }

    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(dns_sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(dns_sock);
        dns_sock = -1;
        return;
    }

    // Set receive timeout so the task can check if it should stop
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    xTaskCreate(dns_task, "dns_server", 4096, NULL, 5, &dns_task_handle);
    ESP_LOGI(TAG, "DNS server started, redirecting to %s", redirect_ip);
}

void dns_server_stop(void)
{
    if (dns_sock >= 0) {
        int s = dns_sock;
        dns_sock = -1;
        close(s);
    }
    if (dns_task_handle) {
        // Task will self-delete when socket is closed
        dns_task_handle = NULL;
    }
}
