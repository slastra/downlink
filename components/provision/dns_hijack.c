/*
 * Minimal DNS responder for the captive portal.
 *
 * IDF's captive_portal example server was tried first and failed twice:
 * it rejects any query carrying an EDNS OPT record (every modern resolver
 * sends one), and its stop path deletes the task and frees the handle the
 * task is still using. This one copies the question verbatim, answers with
 * one A record, ignores everything after the question, and stops by
 * closing its socket and letting the task exit on its own.
 */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dns_hijack.h"

static const char *TAG = "dns";
static volatile int  s_sock = -1;
static uint32_t      s_ip;

static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in from;
    socklen_t fl;
    while (s_sock >= 0) {
        fl = sizeof from;
        int n = recvfrom(s_sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            if (s_sock < 0) break;
            continue;
        }
        /* skip the first question's name: labels until a zero or a pointer */
        int p = 12;
        while (p < n && buf[p] != 0 && (buf[p] & 0xC0) != 0xC0) p += buf[p] + 1;
        if (p >= n - 4) continue;
        p += (buf[p] & 0xC0) == 0xC0 ? 2 : 1;
        uint16_t qtype = (buf[p] << 8) | buf[p + 1];
        int qend = p + 4;
        if (qtype != 1 && qtype != 255) continue;      /* A or ANY only */

        buf[2] = 0x81; buf[3] = 0x80;                  /* response, recursion available */
        buf[4] = 0; buf[5] = 1;                        /* one question */
        buf[6] = 0; buf[7] = 1;                        /* one answer */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;       /* no authority/additional */
        uint8_t *a = buf + qend;
        a[0] = 0xC0; a[1] = 0x0C;                      /* pointer to the question name */
        a[2] = 0; a[3] = 1;                            /* type A */
        a[4] = 0; a[5] = 1;                            /* class IN */
        a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 30;       /* TTL 30 s */
        a[10] = 0; a[11] = 4;
        memcpy(a + 12, &s_ip, 4);
        sendto(s_sock, buf, qend + 16, 0, (struct sockaddr *)&from, fl);
    }
    ESP_LOGI(TAG, "stopped");
    vTaskDelete(NULL);
}

void dns_hijack_start(uint32_t ip_be)
{
    if (s_sock >= 0) return;
    s_ip = ip_be;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "socket: errno %d", errno); return; }
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        ESP_LOGE(TAG, "bind 53: errno %d", errno);
        close(sock);
        return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    s_sock = sock;
    xTaskCreate(dns_task, "dns", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "answering all A queries with the portal address");
}

void dns_hijack_stop(void)
{
    int sock = s_sock;
    if (sock < 0) return;
    s_sock = -1;              /* the task sees this on its next timeout and exits */
    shutdown(sock, 0);
    close(sock);
}
