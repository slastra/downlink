/* Captive-portal DNS: answers every A query with one address. */
#pragma once
#include <stdint.h>
void dns_hijack_start(uint32_t ip_be);   /* network byte order */
void dns_hijack_stop(void);
