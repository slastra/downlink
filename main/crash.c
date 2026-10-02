#include "crash.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa_context.h"

static const char *TAG = "crash";

#define CRASH_MAGIC 0x444c4352u   /* "DLCR" */
#define DEPTH       12

typedef struct {
    uint32_t magic;
    uint32_t sum;                /* over everything after it */
    char     fw[32];
    char     reason[64];
    char     task[16];
    char     wdt[96];            /* task watchdog: who starved, who was running */
    uint8_t  core;
    uint8_t  depth;
    uint8_t  corrupted;
    uint32_t uptime_s;
    uint32_t pc[DEPTH];
} crash_record_t;

static RTC_NOINIT_ATTR crash_record_t s_rec;

/* Everything the handler reads must be in internal RAM: a crash during a
 * flash write runs with the cache (flash and PSRAM) off. */
static DRAM_ATTR char s_fw[32];
static crash_record_t s_last;           /* last boot's crash, if any */
static bool s_have_last;

static uint32_t IRAM_ATTR checksum(const crash_record_t *r)
{
    const uint8_t *p = (const uint8_t *)r + offsetof(crash_record_t, fw);
    uint32_t s = 0x9e3779b9u;
    for (size_t i = 0; i < sizeof *r - offsetof(crash_record_t, fw); i++) s = (s ^ p[i]) * 16777619u;
    return s;
}

/* Copy a string that may live in flash only while flash is readable. */
static void IRAM_ATTR copy_str(char *dst, size_t cap, const char *src)
{
    if (!src || (!esp_ptr_internal(src) && !spi_flash_cache_enabled())) return;
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void IRAM_ATTR append(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    while (n < cap && dst[n]) n++;
    if (n < cap) copy_str(dst + n, cap - n, src);
}

static DRAM_ATTR int s_wdt_msgs;

static void IRAM_ATTR wdt_msg(void *opaque, const char *msg)
{
    if (s_wdt_msgs++ == 0) return;   /* the caption: "Task watchdog got triggered..." */
    append(s_rec.wdt, sizeof s_rec.wdt, msg[0] == '\n' ? (s_rec.wdt[0] ? ", " : "") : msg);
}

/* Runs in the task watchdog's ISR just before it panics: the panic info
 * itself only says "Task watchdog got triggered", not who. */
void IRAM_ATTR esp_task_wdt_isr_user_handler(void)
{
    s_rec.wdt[0] = 0;
    s_wdt_msgs = 0;
    esp_task_wdt_print_triggered_tasks(wdt_msg, NULL, NULL);
    for (int c = 0; c < CONFIG_FREERTOS_NUMBER_OF_CORES; c++) {
        append(s_rec.wdt, sizeof s_rec.wdt, c ? ", " : "; running: ");
        append(s_rec.wdt, sizeof s_rec.wdt, pcTaskGetName(xTaskGetCurrentTaskHandleForCore(c)));
    }
}

void __real_esp_panic_handler(panic_info_t *info);

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    /* Everything but the watchdog list, which the ISR above may have just
     * written (crash_init clears it each boot). */
    s_rec.magic = 0;
    memset(s_rec.fw, 0, offsetof(crash_record_t, wdt) - offsetof(crash_record_t, fw));
    memset(&s_rec.core, 0, sizeof s_rec - offsetof(crash_record_t, core));

    copy_str(s_rec.fw, sizeof s_rec.fw, s_fw);
    /* The task watchdog panics through an illegal instruction, so its own
     * reason would read "IllegalInstruction"; the ISR hook has said who. */
    if (s_rec.wdt[0]) copy_str(s_rec.reason, sizeof s_rec.reason, "task watchdog");
    else if (g_panic_abort && g_panic_abort_details) copy_str(s_rec.reason, sizeof s_rec.reason, g_panic_abort_details);
    else copy_str(s_rec.reason, sizeof s_rec.reason, info->reason);
    s_rec.core = (uint8_t)info->core;
    copy_str(s_rec.task, sizeof s_rec.task, pcTaskGetName(xTaskGetCurrentTaskHandleForCore(info->core)));
    s_rec.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    if (info->frame) {
        const XtExcFrame *xf = (const XtExcFrame *)info->frame;
        esp_backtrace_frame_t f = { .pc = xf->pc, .sp = xf->a1, .next_pc = xf->a0, .exc_frame = (void *)xf };
        s_rec.pc[s_rec.depth++] = esp_cpu_process_stack_pc(f.pc);
        while (s_rec.depth < DEPTH && f.next_pc) {
            if (!esp_backtrace_get_next_frame(&f)) { s_rec.corrupted = 1; break; }
            s_rec.pc[s_rec.depth++] = esp_cpu_process_stack_pc(f.pc);
        }
    }
    s_rec.sum = checksum(&s_rec);
    s_rec.magic = CRASH_MAGIC;

    __real_esp_panic_handler(info);
}

void crash_init(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
    if (crashed && s_rec.magic == CRASH_MAGIC && s_rec.sum == checksum(&s_rec)) {
        s_last = s_rec;
        s_have_last = true;
        s_last.fw[sizeof s_last.fw - 1] = s_last.reason[sizeof s_last.reason - 1] = 0;
        s_last.task[sizeof s_last.task - 1] = s_last.wdt[sizeof s_last.wdt - 1] = 0;
        if (s_last.depth > DEPTH) s_last.depth = DEPTH;
        ESP_LOGE(TAG, "last boot crashed: %s (task %s, core %u, %lu s up, fw %s)%s%s",
                 s_last.reason, s_last.task, s_last.core, (unsigned long)s_last.uptime_s, s_last.fw,
                 s_last.wdt[0] ? "; watchdog:" : "", s_last.wdt);
    }
    s_rec.magic = 0;
    s_rec.wdt[0] = 0;
    strlcpy(s_fw, esp_app_get_description()->version, sizeof s_fw);
}

void crash_add_json(cJSON *status)
{
    if (!s_have_last) return;
    cJSON *c = cJSON_AddObjectToObject(status, "crash");
    cJSON_AddStringToObject(c, "fw", s_last.fw);
    cJSON_AddStringToObject(c, "reason", s_last.reason);
    cJSON_AddStringToObject(c, "task", s_last.task);
    cJSON_AddNumberToObject(c, "core", s_last.core);
    cJSON_AddNumberToObject(c, "uptimeS", s_last.uptime_s);
    char bt[DEPTH * 11 + 16] = "";
    for (int i = 0; i < s_last.depth; i++) {
        char pc[12];
        snprintf(pc, sizeof pc, "%s0x%08lx", i ? " " : "", (unsigned long)s_last.pc[i]);
        strlcat(bt, pc, sizeof bt);
    }
    if (s_last.corrupted) strlcat(bt, " |<-CORRUPTED", sizeof bt);
    cJSON_AddStringToObject(c, "backtrace", bt);
    if (s_last.wdt[0]) cJSON_AddStringToObject(c, "wdt", s_last.wdt);
}
