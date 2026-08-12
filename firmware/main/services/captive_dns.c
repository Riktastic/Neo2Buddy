/**
 * @file captive_dns.c
 * @brief Tiny DNS responder that redirects all queries to the AP gateway IP.
 *
 * Used only during first-run onboarding SoftAP (and recovery hotspot) so phones
 * open the portal. Stopped once onboarding is complete on a normal SoftAP.
 */

#include "captive_dns.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_netif.h"
#include <string.h>
#include <stdlib.h>
#include "log_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "captive_dns";
static int s_dns_sock = -1;
static volatile bool s_dns_run = false;
static TaskHandle_t s_dns_task = NULL;

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
        s_dns_run = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int rc = bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        ESP_LOGE(TAG, "bind failed");
        closesocket(s_dns_sock);
        s_dns_sock = -1;
        s_dns_run = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Non-blocking-ish receive timeout so stop() can unwind the loop. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS responder started on port 53");
    log_buffer_appendf("captive_dns: started on port 53");

    while (s_dns_run) {
        uint8_t buf[512];
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int r = recvfrom(s_dns_sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &len);
        if (r <= 0) {
            continue;
        }
        if ((size_t)r < 12) {
            continue;
        }

        uint8_t resp[512];
        memcpy(resp, buf, r);
        resp[2] |= 0x80;
        resp[3] &= ~0x01;
        resp[6] = 0x00;
        resp[7] = 0x01;

        size_t out_len = (size_t)r;
        uint8_t answer[] = { 0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 30, 0x00, 0x04, 192, 168, 4, 1 };
        if (out_len + sizeof(answer) < sizeof(resp)) {
            memcpy(resp + out_len, answer, sizeof(answer));
            out_len += sizeof(answer);
            sendto(s_dns_sock, resp, out_len, 0, (struct sockaddr *)&cli, len);
        }
    }

    if (s_dns_sock >= 0) {
        closesocket(s_dns_sock);
        s_dns_sock = -1;
    }
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(void)
{
    if (s_dns_run || s_dns_task != NULL) {
        return ESP_OK;
    }
    s_dns_run = true;
    if (xTaskCreate(dns_task, "captive_dns", 4096, NULL, 5, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        s_dns_task = NULL;
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    log_buffer_appendf("captive_dns: task created");
    return ESP_OK;
}

esp_err_t captive_dns_stop(void)
{
    if (!s_dns_run && s_dns_sock < 0) {
        return ESP_OK;
    }
    s_dns_run = false;
    if (s_dns_sock >= 0) {
        closesocket(s_dns_sock);
        s_dns_sock = -1;
    }
    /* Task deletes itself after the recv timeout / loop exit. */
    return ESP_OK;
}
