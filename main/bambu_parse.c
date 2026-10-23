// main/bambu_parse.c — 拓竹 MQTT report.print 解析(纯 C + cJSON,有主机回放测试 tests/test_replay.c)
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "bambu_parse.h"

void bambu_parse_print(bambu_parse_ctx_t *ctx, const cJSON *p, printer_status_t *st) {
    const cJSON *v;
    st->valid = true;
    if (cJSON_IsString(v = cJSON_GetObjectItem(p, "gcode_state")))
        snprintf(st->gcode_state, sizeof st->gcode_state, "%s", v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "mc_percent"))) st->percent = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "mc_remaining_time"))) st->remaining_min = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "layer_num"))) st->layer = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "total_layer_num"))) st->total_layer = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "nozzle_temper"))) st->nozzle = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "nozzle_target_temper"))) st->nozzle_target = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "bed_temper"))) st->bed = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "bed_target_temper"))) st->bed_target = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(p, "print_error"))) st->print_error = (uint32_t)v->valuedouble;
    if (cJSON_IsString(v = cJSON_GetObjectItem(p, "subtask_id")) && v->valuestring[0] && strcmp(v->valuestring, "0"))
        snprintf(st->job_id, sizeof st->job_id, "%s", v->valuestring);
    else if (cJSON_IsString(v = cJSON_GetObjectItem(p, "task_id")) && v->valuestring[0] && strcmp(v->valuestring, "0"))
        snprintf(st->job_id, sizeof st->job_id, "%s", v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(p, "subtask_name")))
        snprintf(st->subtask, sizeof st->subtask, "%s", v->valuestring);
    // 单喷嘴机型:顶层 nozzle_temper 就是唯一喷嘴(UI 序号 0)
    if (!ctx->h2 && (cJSON_GetObjectItem(p, "nozzle_temper") || cJSON_GetObjectItem(p, "nozzle_target_temper"))) {
        st->n_nozzles = 1; st->active_nozzle = 0;
        st->noz[0] = st->nozzle; st->noz_t[0] = st->nozzle_target;
    }
    // H2 系列(H2D/H2C 双喷嘴):device.extruder.info[] 每个喷嘴一项,temp 打包 = 低 16 位当前 | 高 16 位目标;
    // 在用喷嘴 = (extruder.state >> 4) & 0xF(与 ha-bambulab 一致)。extruder id 0 = 右喷嘴, 1 = 左喷嘴。
    // snow = 该喷嘴当前料槽 (ams_id << 8 | slot)。有它就覆盖旧的 nozzle_temper。
    const cJSON *dev = cJSON_GetObjectItem(p, "device");
    const cJSON *exo = dev ? cJSON_GetObjectItem(dev, "extruder") : NULL;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(exo, "state"))) ctx->active_nozzle = ((uint32_t)v->valuedouble >> 4) & 0xF;
    const cJSON *ext = exo ? cJSON_GetObjectItem(exo, "info") : NULL;
    if (cJSON_IsArray(ext)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, ext) {
            const cJSON *id = cJSON_GetObjectItem(e, "id");
            if (!cJSON_IsNumber(id) || id->valueint < 0 || id->valueint > 1) continue;
            int ui = id->valueint == 0 ? 1 : 0;              // 拓竹 id 0 = 右 -> UI 1
            if (id->valueint == 1) ctx->h2 = true;
            if (cJSON_IsNumber(v = cJSON_GetObjectItem(e, "temp"))) {
                uint32_t t = (uint32_t)v->valuedouble;
                st->noz[ui] = (float)(t & 0xFFFF); st->noz_t[ui] = (float)(t >> 16);
            }
            if (cJSON_IsNumber(v = cJSON_GetObjectItem(e, "snow"))) ctx->snow[id->valueint] = (int)v->valuedouble;
        }
    }
    if (ctx->h2) {
        int ui = ctx->active_nozzle == 0 ? 1 : 0;
        st->n_nozzles = 2; st->active_nozzle = ui;
        st->nozzle = st->noz[ui]; st->nozzle_target = st->noz_t[ui];
    }
    const cJSON *bed = dev ? cJSON_GetObjectItem(cJSON_GetObjectItem(dev, "bed"), "info") : NULL;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(bed, "temp"))) {
        uint32_t t = (uint32_t)v->valuedouble;
        st->bed = (float)(t & 0xFFFF); st->bed_target = (float)(t >> 16);
    }
    // 仓温:H2 = device.ctc.info.temp(同样打包);X1 = 顶层 chamber_temper;P1/A1 没有仓温传感器
    const cJSON *ctc = dev ? cJSON_GetObjectItem(cJSON_GetObjectItem(dev, "ctc"), "info") : NULL;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(ctc, "temp"))) {
        uint32_t t = (uint32_t)v->valuedouble;
        st->has_chamber = true; st->chamber = (float)(t & 0xFFFF); st->chamber_target = (float)(t >> 16); st->chamber_has_target = true;
    } else if (!ctx->h2 && cJSON_IsNumber(v = cJSON_GetObjectItem(p, "chamber_temper"))) {
        st->has_chamber = true; st->chamber = (float)v->valuedouble; st->chamber_target = 0; st->chamber_has_target = false;
    }
    // 当前料槽:H2 用在用喷嘴的 snow;其他机型用 ams.tray_now = 全局槽号(ams*4+slot);254/255 = 外挂料盘
    const cJSON *ams = cJSON_GetObjectItem(p, "ams");
    if (cJSON_IsObject(ams)) {
        const cJSON *tn = cJSON_GetObjectItem(ams, "tray_now");
        if (cJSON_IsString(tn)) ctx->tray_now = atoi(tn->valuestring);
    }
    int ams_id = -1, slot = -1;
    if (ctx->h2 && ctx->snow[ctx->active_nozzle] >= 0) {
        int sn = ctx->snow[ctx->active_nozzle];
        ams_id = sn >> 8; slot = sn & 0xFF;
        // 低字节 0xFF = 没有料(0xFEFF/0xFFFF/…);254/255 + slot 0 = 左/右外挂。旧条件把 0xFEFF 当成外挂、把 0xFF00 清掉。
        if (slot == 255) ams_id = -1;
    } else if (ctx->tray_now == 254 || ctx->tray_now == 255) { ams_id = ctx->tray_now; slot = 0; }
    else if (ctx->tray_now >= 0 && ctx->tray_now < 128) { ams_id = ctx->tray_now / 4; slot = ctx->tray_now % 4; }
    const cJSON *units = cJSON_IsObject(ams) ? cJSON_GetObjectItem(ams, "ams") : NULL;
    if (ams_id >= 0 && ams_id < 254 && cJSON_IsArray(units)) {
        const cJSON *u;
        cJSON_ArrayForEach(u, units) {
            const cJSON *id = cJSON_GetObjectItem(u, "id");
            if (!cJSON_IsString(id) || atoi(id->valuestring) != ams_id) continue;
            const cJSON *hr = cJSON_GetObjectItem(u, "humidity_raw");
            if (cJSON_IsString(hr) && hr->valuestring[0]) st->fil_humidity = atoi(hr->valuestring);
            const cJSON *t;
            cJSON_ArrayForEach(t, cJSON_GetObjectItem(u, "tray")) {
                const cJSON *tid = cJSON_GetObjectItem(t, "id");
                if (!cJSON_IsString(tid) || atoi(tid->valuestring) != slot) continue;
                const cJSON *col = cJSON_GetObjectItem(t, "tray_color"), *ty = cJSON_GetObjectItem(t, "tray_type");
                if (cJSON_IsString(col)) st->fil_rgb = pet_parse_tray_color(col->valuestring);
                if (cJSON_IsString(ty)) snprintf(st->fil_type, sizeof st->fil_type, "%s", ty->valuestring);
                pet_tray_label(ams_id, slot, st->fil_slot, sizeof st->fil_slot);
            }
        }
    }
    if (ams_id == 254 || ams_id == 255) {
        // 外挂料盘:H2 在 vir_slot[](id 254 = 左,255 = 右),其他机型在 vt_tray
        const cJSON *vt = cJSON_GetObjectItem(p, "vt_tray");
        const cJSON *vs = cJSON_GetObjectItem(p, "vir_slot");
        if (ctx->h2 && cJSON_IsArray(vs)) {
            const char *want = ams_id == 255 ? "255" : "254";
            const cJSON *x;
            cJSON_ArrayForEach(x, vs) { const cJSON *id = cJSON_GetObjectItem(x, "id"); if (cJSON_IsString(id) && !strcmp(id->valuestring, want)) vt = x; }
        }
        if (cJSON_IsObject(vt)) {
            const cJSON *col = cJSON_GetObjectItem(vt, "tray_color"), *ty = cJSON_GetObjectItem(vt, "tray_type");
            if (cJSON_IsString(col)) st->fil_rgb = pet_parse_tray_color(col->valuestring);
            if (cJSON_IsString(ty)) snprintf(st->fil_type, sizeof st->fil_type, "%s", ty->valuestring);
            pet_tray_label(ams_id, 0, st->fil_slot, sizeof st->fil_slot);
            st->fil_humidity = -1;
        }
    }
    const cJSON *hms = cJSON_GetObjectItem(p, "hms");
    if (cJSON_IsArray(hms)) {
        bool runout = false;
        const cJSON *it;
        cJSON_ArrayForEach(it, hms) {
            const cJSON *a = cJSON_GetObjectItem(it, "attr"), *c = cJSON_GetObjectItem(it, "code");
            if (cJSON_IsNumber(a) && cJSON_IsNumber(c) &&
                pet_is_runout_hms((uint32_t)a->valuedouble, (uint32_t)c->valuedouble)) runout = true;
        }
        st->hms_runout = runout;
    }
}


// ---- v4.3 耗材页 ----
static int jint(const cJSON *v, int dflt) {
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring[0]) return atoi(v->valuestring);
    return dflt;
}
// 一个料槽对象(AMS tray / vt_tray / vir_slot 项)合并进 t。loaded 判定和 ha-bambulab 一致:
// 有 state 时看 bit0(有料盘)且(state<=3 时 ==3,否则 bit3 稳定);没有 state 时看 tray_type 是否非空。
static void merge_tray(const cJSON *o, pet_tray_t *t) {
    const cJSON *ty = cJSON_GetObjectItem(o, "tray_type"), *col = cJSON_GetObjectItem(o, "tray_color");
    const cJSON *rem = cJSON_GetObjectItem(o, "remain"), *state = cJSON_GetObjectItem(o, "state");
    if (cJSON_IsString(ty)) {
        snprintf(t->type, sizeof t->type, "%s", ty->valuestring);
        if (!strcmp(t->type, "Empty")) t->type[0] = 0;
    }
    if (cJSON_IsString(col)) {
        uint32_t c = pet_parse_tray_color(col->valuestring);
        t->r = c >> 16; t->g = (c >> 8) & 0xFF; t->b = c & 0xFF;
    }
    if (cJSON_IsNumber(rem)) { int r = rem->valueint; t->remain = (int8_t)(r < -1 ? -1 : r > 100 ? 100 : r); }
    bool loaded;
    if (cJSON_IsNumber(state) || cJSON_IsString(state)) {
        int sv = jint(state, 0);
        loaded = (sv & 1) && (sv <= 3 ? sv == 3 : (sv & 8) != 0);
        // 只有 state、没有类型时(增量报文)保留原来的空/非空判断,除非 state 明确说没有料盘
        if (!cJSON_IsString(ty) && (sv & 1) && (t->flags & PET_TRAY_KNOWN)) loaded = !(t->flags & PET_TRAY_EMPTY);
    } else if (cJSON_IsString(ty)) loaded = t->type[0] != 0;
    else if (t->flags & PET_TRAY_KNOWN) loaded = !(t->flags & PET_TRAY_EMPTY);
    else loaded = false;                               // 只有 id:空槽
    t->flags = (t->flags & ~PET_TRAY_EMPTY) | PET_TRAY_KNOWN | (loaded ? 0 : PET_TRAY_EMPTY);
    if (!loaded) t->remain = -1;
}
static int exist_bit(int id, int slot) {
    if (id >= 0 && id < 4) return id * 4 + slot;
    if (pet_ams_is_ht(id)) return 16 + (id - 128);
    return -1;
}
static int unit_cmp_key(int id) { return pet_ams_is_ht(id) ? 1000 + id : id; }

void bambu_parse_ams(bambu_parse_ctx_t *ctx, const cJSON *p, pet_ams_t *a) {
    const cJSON *ams = cJSON_GetObjectItem(p, "ams");
    const cJSON *units = cJSON_IsObject(ams) ? cJSON_GetObjectItem(ams, "ams") : NULL;
    if (cJSON_IsObject(ams)) {
        const cJSON *eb = cJSON_GetObjectItem(ams, "tray_exist_bits");
        if (cJSON_IsString(eb) && eb->valuestring[0]) { ctx->exist_bits = (uint32_t)strtoul(eb->valuestring, NULL, 16); ctx->has_exist = true; }
    }
    if (cJSON_IsArray(units)) {
        // 报文里的 ams[] 是完整的机组列表:按 id 重建(普通 AMS 在前,HT 在后),已有机组保留旧的料槽数据再合并
        int ids[PET_AMS_MAX_UNITS], n = 0;
        const cJSON *u;
        cJSON_ArrayForEach(u, units) {
            int id = jint(cJSON_GetObjectItem(u, "id"), -1);
            if (id < 0 || id > 255 || n >= PET_AMS_MAX_UNITS) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) dup |= ids[k] == id;
            if (!dup) ids[n++] = id;
        }
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && unit_cmp_key(ids[j]) < unit_cmp_key(ids[j - 1]); j--) { int t = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = t; }
        pet_ams_t old = *a;                             // 约 400 B,在 MQTT 任务栈上(6 KB)
        a->n_units = 0; a->n_slots = 0;
        for (int i = 0; i < n; i++) {
            int ns = pet_ams_is_ht(ids[i]) ? 1 : 4;
            if (a->n_slots + ns > PET_AMS_MAX_SLOTS) break;
            pet_ams_unit_t *nu = &a->u[a->n_units++];
            *nu = (pet_ams_unit_t){ (uint8_t)ids[i], -1, -1, a->n_slots, (uint8_t)ns };
            for (int s = 0; s < ns; s++) { a->slot[nu->first + s] = (pet_tray_t){ 0 }; a->slot[nu->first + s].remain = -1; }
            for (int k = 0; k < old.n_units; k++) {
                if (old.u[k].id != ids[i]) continue;
                nu->hum_pct = old.u[k].hum_pct; nu->hum_lvl = old.u[k].hum_lvl;
                for (int s = 0; s < ns && s < old.u[k].nslot; s++) a->slot[nu->first + s] = old.slot[old.u[k].first + s];
            }
            a->n_slots += ns;
        }
        cJSON_ArrayForEach(u, units) {
            int id = jint(cJSON_GetObjectItem(u, "id"), -1);
            pet_ams_unit_t *nu = NULL;
            for (int k = 0; k < a->n_units; k++) if (a->u[k].id == id) nu = &a->u[k];
            if (!nu) continue;
            int h = jint(cJSON_GetObjectItem(u, "humidity_raw"), -1);
            if (cJSON_GetObjectItem(u, "humidity_raw")) nu->hum_pct = (int8_t)(h >= 1 && h <= 100 ? h : -1);
            int lv = jint(cJSON_GetObjectItem(u, "humidity"), -1);
            if (cJSON_GetObjectItem(u, "humidity")) nu->hum_lvl = (int8_t)(lv >= 1 && lv <= 5 ? lv : -1);
            const cJSON *t;
            cJSON_ArrayForEach(t, cJSON_GetObjectItem(u, "tray")) {
                int s = jint(cJSON_GetObjectItem(t, "id"), -1);
                if (s < 0 || s >= nu->nslot) continue;
                merge_tray(t, &a->slot[nu->first + s]);
            }
        }
        a->valid = true;
    }
    if (ctx->has_exist) {
        for (int k = 0; k < a->n_units; k++)
            for (int s = 0; s < a->u[k].nslot; s++) {
                int b = exist_bit(a->u[k].id, s);
                pet_tray_t *t = &a->slot[a->u[k].first + s];
                if (b >= 0 && !(ctx->exist_bits >> b & 1)) { t->flags |= PET_TRAY_KNOWN | PET_TRAY_EMPTY; t->remain = -1; }
            }
    }
    // 外挂料盘:H2 = vir_slot[](id 254 = 左, 255 = 右,对应 ha-bambulab external_spool[1]/[0]);其他机型 = vt_tray
    const cJSON *vs = cJSON_GetObjectItem(p, "vir_slot"), *vt = cJSON_GetObjectItem(p, "vt_tray");
    if (cJSON_IsArray(vs)) {
        pet_tray_t old[PET_AMS_MAX_EXT]; int on = a->n_ext;
        memcpy(old, a->ext, sizeof old);
        pet_tray_t nx[PET_AMS_MAX_EXT]; int n = 0;
        for (int side = 254; side <= 255; side++) {
            const cJSON *x;
            cJSON_ArrayForEach(x, vs) {
                if (jint(cJSON_GetObjectItem(x, "id"), -1) != side || n >= PET_AMS_MAX_EXT) continue;
                uint8_t sf = side == 254 ? PET_TRAY_SIDE_L : PET_TRAY_SIDE_R;
                pet_tray_t t = { 0 }; t.remain = -1;
                for (int k = 0; k < on; k++) if (old[k].flags & sf) t = old[k];
                merge_tray(x, &t);
                t.flags |= sf; t.remain = -1;            // 外挂料盘没有余量
                nx[n++] = t;
                break;
            }
        }
        memcpy(a->ext, nx, sizeof nx); a->n_ext = (uint8_t)n;
    } else if (cJSON_IsObject(vt) && !(a->n_ext && (a->ext[0].flags & (PET_TRAY_SIDE_L | PET_TRAY_SIDE_R)))) {
        if (!a->n_ext) { a->ext[0] = (pet_tray_t){ 0 }; a->n_ext = 1; }
        merge_tray(vt, &a->ext[0]);
        a->ext[0].remain = -1;
    }
    // 在用:H2 两个喷嘴的 snow 都标;其他机型 tray_now(0..127 = ams*4+slot,254/255 = 外挂)
    int sn[2] = { -1, -1 }, k = 0;
    if (ctx->h2) { sn[0] = ctx->snow[0]; sn[1] = ctx->snow[1]; k = 2; }
    else if (ctx->tray_now == 254 || ctx->tray_now == 255) { sn[0] = 255 << 8; k = 1; }
    else if (ctx->tray_now >= 0 && ctx->tray_now < 128) { sn[0] = ((ctx->tray_now / 4) << 8) | (ctx->tray_now % 4); k = 1; }
    pet_ams_mark_active(a, sn, k);
}
