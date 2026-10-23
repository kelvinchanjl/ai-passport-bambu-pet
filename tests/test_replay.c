// tests/test_replay.c — 把真实 H2D 报文(及按字段改出的变体)逐条喂给固件真实解析器 + 状态机,打印每条结果。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "bambu_parse.h"

static const char *NAMES[] = { "IDLE", "PRINT", "DONE", "PAUSE", "EMPTY", "FAIL" };

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/fixtures/h2d_replay.jsonl";
    FILE *f = fopen(path, "r"); assert(f);
    static char line[64 * 1024];
    printer_status_t st; printer_status_reset(&st);
    bambu_parse_ctx_t ctx = BAMBU_PARSE_CTX_INIT;
    pet_machine_t m; pet_machine_init(&m);
    pet_job_t job; pet_job_init(&job);              // 打印用时:每条报文假定间隔 10 分钟
    int64_t now = 1760000000; int done_elapsed = -2;
    int n = 0, seen[PET_STATE_COUNT] = {0};
    while (fgets(line, sizeof line, f)) {
        cJSON *j = cJSON_Parse(line); assert(j);
        const char *note = cJSON_GetObjectItem(j, "note")->valuestring;
        const cJSON *msg = cJSON_GetObjectItem(j, "msg");
        pet_state_t s;
        if (cJSON_IsNull(msg)) { pet_machine_ack(&m); s = pet_machine_update(&m, &st); }
        else {
            char *raw = cJSON_PrintUnformatted(msg); size_t len = strlen(raw);
            cJSON *root = cJSON_ParseWithLength(raw, len);           // 和 handle_json 一样走一遍完整解析
            const cJSON *p = cJSON_GetObjectItem(root, "print");
            if (cJSON_IsObject(p)) bambu_parse_print(&ctx, p, &st);
            cJSON_Delete(root); free(raw);
            now += 600;
            pet_job_update(&job, &st, now);
            st.elapsed_min = pet_job_elapsed_min(&job, &st);
            s = pet_machine_update(&m, &st);
        }
        if (s == PET_DONE && done_elapsed == -2) done_elapsed = st.elapsed_min;
        seen[s]++;
        const cJSON *ex = cJSON_GetObjectItem(j, "expect");
        if (!cJSON_IsString(ex) || strcmp(ex->valuestring, NAMES[s])) { fprintf(stderr, "#%d expected %s got %s\n", n + 1, ex ? ex->valuestring : "?", NAMES[s]); return 1; }
        printf("#%02d %-5s | gcode=%-7s pct=%3d rem=%4d layer=%d/%d noz=%.0f/%.0f(idx%d) bed=%.0f/%.0f err=0x%08X hms_runout=%d rgb=%06X | %s\n",
               ++n, NAMES[s], st.gcode_state, st.percent, st.remaining_min, st.layer, st.total_layer, st.nozzle, st.nozzle_target,
               ctx.active_nozzle, st.bed, st.bed_target, (unsigned)st.print_error, st.hms_runout, (unsigned)st.fil_rgb, note);
        cJSON_Delete(j);
    }
    fclose(f);
    for (int i = 0; i < PET_STATE_COUNT; i++) { printf("%s=%d ", NAMES[i], seen[i]); assert(seen[i] > 0); }
    printf("\nreplay: all 6 states reached (%d messages); job %s elapsed at DONE = %d min\n", n, job.job, done_elapsed);
    assert(job.job[0]);                 // subtask_id parsed from the real push_all
    assert(done_elapsed > 0);           // job seen from 0% -> 用时 known at FINISH
    return 0;
}
