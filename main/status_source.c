// main/status_source.c — 状态存储(多机)与演示源。
#include "status_source.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define ONLINE_WINDOW_MS (5 * 60 * 1000)
typedef struct {
    int n;
    printer_status_t st[PET_MAX_PRINTERS];
    printer_info_t info[PET_MAX_PRINTERS];
    TickType_t last_rx[PET_MAX_PRINTERS];      // 0 = 从没收到
    link_state_t link;
} src_data_t;

static SemaphoreHandle_t s_mu;
static pet_ams_t s_cloud_ams[PET_MAX_PRINTERS];      // v4.3:只有云端存 AMS(演示数据是常量,按需生成)
static src_data_t s_src[2];                     // [SRC_DEMO], [SRC_CLOUD]
static volatile uint32_t s_ver;
static status_src_t s_active = SRC_DEMO;
static int s_cur;

void status_store_init(void) {
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    for (int k = 0; k < 2; k++) {
        memset(&s_src[k], 0, sizeof s_src[k]);
        s_src[k].n = 1;
        for (int i = 0; i < PET_MAX_PRINTERS; i++) printer_status_reset(&s_src[k].st[i]);
    }
    for (int i = 0; i < PET_MAX_PRINTERS; i++) pet_ams_reset(&s_cloud_ams[i]);
    s_src[SRC_DEMO].link = LINK_DEMO;
    s_src[SRC_CLOUD].link = LINK_UNCONFIGURED;
}
void status_store_get_idx(int idx, printer_status_t *out, link_state_t *link, bool *online, printer_info_t *info) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const src_data_t *d = &s_src[s_active];
    if (idx < 0 || idx >= d->n) idx = 0;
    if (out) *out = d->st[idx];
    if (link) *link = d->link;
    if (info) *info = d->info[idx];
    if (online) {
        if (s_active == SRC_DEMO) *online = true;
        else *online = d->link == LINK_ONLINE && d->last_rx[idx] &&
                       (xTaskGetTickCount() - d->last_rx[idx]) < pdMS_TO_TICKS(ONLINE_WINDOW_MS);
    }
    xSemaphoreGive(s_mu);
}
void status_store_get(printer_status_t *out, link_state_t *link) { status_store_get_idx(s_cur, out, link, NULL, NULL); }
int status_store_count(void) { return s_src[s_active].n; }
int status_store_current(void) { return s_cur < s_src[s_active].n ? s_cur : 0; }
void status_store_select(int idx) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (idx >= 0 && idx < s_src[s_active].n) { s_cur = idx; s_ver++; }
    xSemaphoreGive(s_mu);
}
void status_store_set_idx(status_src_t src, int idx, const printer_status_t *s) {
    if (idx < 0 || idx >= PET_MAX_PRINTERS) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_src[src].st[idx] = *s;
    s_src[src].last_rx[idx] = s->valid ? (xTaskGetTickCount() | 1) : 0;
    if (src == s_active) s_ver++;
    xSemaphoreGive(s_mu);
}
void status_store_set(status_src_t src, const printer_status_t *s) { status_store_set_idx(src, 0, s); }
void status_store_set_info(status_src_t src, int n, const printer_info_t *info) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (n < 1) n = 1;
    if (n > PET_MAX_PRINTERS) n = PET_MAX_PRINTERS;
    s_src[src].n = n;
    for (int i = 0; i < n; i++) s_src[src].info[i] = info[i];
    if (src == s_active) { if (s_cur >= n) s_cur = 0; s_ver++; }
    xSemaphoreGive(s_mu);
}
void status_store_set_link(status_src_t src, link_state_t l) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_src[src].link = src == SRC_DEMO ? LINK_DEMO : l;
    if (src == s_active) s_ver++;
    xSemaphoreGive(s_mu);
}
void status_store_activate(status_src_t src) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_active = src;
    if (s_cur >= s_src[src].n) s_cur = 0;
    s_ver++;
    xSemaphoreGive(s_mu);
}
pet_ams_t *status_store_ams_lock(int idx) {
    if (idx < 0 || idx >= PET_MAX_PRINTERS) return NULL;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    return &s_cloud_ams[idx];
}
void status_store_ams_unlock(void) {
    if (s_active == SRC_CLOUD) s_ver++;
    xSemaphoreGive(s_mu);
}
void status_store_get_ams(int idx, pet_ams_t *out) {
    xSemaphoreTake(s_mu, portMAX_DELAY);
    status_src_t src = s_active;
    if (idx < 0 || idx >= s_src[src].n) idx = 0;
    if (src == SRC_CLOUD) *out = s_cloud_ams[idx];
    xSemaphoreGive(s_mu);
    if (src == SRC_DEMO) pet_fake_ams(idx, out);
}
status_src_t status_store_active(void) { return s_active; }
uint32_t status_store_version(void) { return s_ver; }

// v4.4:演示状态不再自动轮换,只有单击中键才下一个(demo_next 唤醒任务立即刷新)
static TaskHandle_t s_demo;
static volatile bool s_demo_run;
static volatile int s_step;

static void demo_task(void *arg) {
    (void)arg;
    uint32_t tick = 0;
    static const printer_info_t INFO[1] = { { "我的 H2C", "H2C" } };   // v4.4:演示只有一台(去掉示例 P1S)
    status_store_set_info(SRC_DEMO, 1, INFO);
    while (s_demo_run) {
        printer_status_t st;
        pet_fake_status(s_step, tick, &st);
        status_store_set_idx(SRC_DEMO, 0, &st);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));   // 按中键时被 demo_next 提前唤醒
        tick++;
    }
    s_demo = NULL;
    vTaskDelete(NULL);
}
void demo_source_start(void) {
    if (s_demo) return;
    s_demo_run = true; s_step = 0;
    xTaskCreate(demo_task, "demo_src", 3072, NULL, 3, &s_demo);
}
void demo_source_stop(void) { s_demo_run = false; }
void demo_next(void) { s_step = (s_step + 1) % 6; TaskHandle_t t = s_demo; if (t) xTaskNotifyGive(t); }   // 空闲→打印中→完成→暂停→断料→失败→循环
int demo_step(void) { return s_step; }
bool demo_source_running(void) { return s_demo != NULL && s_demo_run; }
