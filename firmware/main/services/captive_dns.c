/**
 * @file captive_dns.c
 * @brief Tiny DNS responder that redirects all queries to the AP gateway IP.
 *
 * This implements a minimal UDP server that answers A record queries with
 * the AP IP (typically 192.168.4.1). It's sufficient for simple captive
 * portal behavior in development. Production-grade captive portals require
 * a more complete DNS implementation and security considerations.
 */

#include "captive_dns.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_netif.h"
#include <string.h>
#include <stdlib.h>
#include "log_buffer.h"

static const char *TAG = "captive_dns";
static int s_dns_sock = -1;

static void dns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int rc = bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        ESP_LOGE(TAG, "bind failed");
        closesocket(s_dns_sock);
        s_dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS responder started on port 53");
    log_buffer_appendf("captive_dns: started on port 53");

    while (true) {
        uint8_t buf[512];
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int r = recvfrom(s_dns_sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &len);
        if (r <= 0) continue;

        /* Basic parsing: copy transaction ID and flags, set response bit, and
         * append a simple answer with A record to 192.168.4.1. This is not a
         * full parser and assumes typical small queries. */
        if ((size_t)r < 12) continue; /* DNS header size */

        uint8_t resp[512];
        memcpy(resp, buf, r);
        /* Set QR bit (response) and clear recursion desired in flags */
        resp[2] |= 0x80;
        resp[3] &= ~0x01;
        /* Answer count = 1 */
        resp[6] = 0x00; resp[7] = 0x01;

        /* Append answer: pointer to name (0xc0 0x0c), type A (0x0001), class IN,
         * TTL 30, RDLENGTH 4, RDATA 192.168.4.1 */
        size_t out_len = r;
        uint8_t answer[] = { 0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 30, 0x00, 0x04, 192,168,4,1 };
        if (out_len + sizeof(answer) < sizeof(resp)) {
            memcpy(resp + out_len, answer, sizeof(answer));
            out_len += sizeof(answer);
            sendto(s_dns_sock, resp, out_len, 0, (struct sockaddr *)&cli, len);
        }
    }
}

esp_err_t captive_dns_start(void)
{
    if (s_dns_sock >= 0) return ESP_OK;
    xTaskCreate(dns_task, "captive_dns", 4096, NULL, 5, NULL);
    log_buffer_appendf("captive_dns: task created");
    return ESP_OK;
}

esp_err_t captive_dns_stop(void)
{
    if (s_dns_sock >= 0) {
        closesocket(s_dns_sock);
        s_dns_sock = -1;
    }
    return ESP_OK;
}
