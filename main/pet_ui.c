#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
// main/pet_ui.c — 自绘界面:宠物精灵(可换色"呆毛")、代码绘制逐层长高的 Benchy、
// 状态文案、淡入淡出过渡、爱心互动、数据页。没有复用 demo 菜单/外壳。
#include "pet_ui.h"
#include "status_bar.h"
#include "assets/pet_frames.h"
#include "assets/benchy.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(font_pet_8);    // v4.3 只有 L/R,耗材页在用标签
LV_FONT_DECLARE(font_pet_12);
LV_FONT_DECLARE(font_pet_14);
LV_FONT_DECLARE(font_pet_15);
LV_FONT_DECLARE(font_pet_16);   // v4.4.1 连接中/未连上 标题   // v4.4.1 详情页温度当前值(只有数字和 -)
LV_FONT_DECLARE(font_pet_18);
LV_FONT_DECLARE(font_pet_24);
LV_FONT_DECLARE(font_pet_36);
LV_IMAGE_DECLARE(img_heart);
LV_IMAGE_DECLARE(img_ico_nozzle);
LV_IMAGE_DECLARE(img_ico_bed);
LV_IMAGE_DECLARE(img_ico_chamber);
extern const uint8_t pet_frames_bin[];

#define COL_TEXT   0xF4F4F8
#define COL_MUTED  0x8C92A0
#define COL_ORANGE 0xFF8C3C
#define COL_MINT   0x00BC44
#define COL_TRACK  0x282C3A
#define COL_WHITE  0xFFFFFF
#define COL_FAIL_BORDER 0xFFF0C8   // 与美术帧一致
// v4.1 布局(按 B5 效果图):顶部绿色状态标题 / 中间角色(约 130px 高,居中)/ 下方大字主信息 / 最底一行灰色数据。
// 屏幕坐标;tools/pet/layout_check.py 镜像这组数字逐帧检查文字不压角色。
#define CANVAS_W   150
#define CANVAS_H   200
#define CANVAS_X   (120 - CANVAS_W / 2)
// v4.2:顶部多了机器名 + 在线绿点(HDR_Y)和页码小圆点(DOTS_Y),状态标题和角色整体下移。
#define HDR_Y      22          // 机器名(14px)中心线
#define DOTS_Y     40          // 页码小圆点
#define CANVAS_Y   22          // 画布底边(角色脚)在 y=222
#define TITLE_Y    64          // 绿色状态标题(18px)
#define MAIN_Y     250         // 大字主信息(18px;打印中百分比 24px)
#define BAR_Y      270         // 打印中进度条
#define DATA_Y     298         // 灰色数据行(14px)
#define DEMO_X     166         // v4.2.1 演示角标「n/6」(12px,右对齐到 x=226)
#define DEMO_Y     46
#define COL_TITLE  0x00AE42
#define COL_DATA   0x8C92A0
// 打印中的热床和挤出线已经画在帧里;固件不再画 Benchy/热床/挤出线,避免双热床。
// v4.2.1:printing/meta.json 的 nozzle_tip 含义改为「喷嘴出料口 = 挤出线顶端」(头到喷嘴的线已从帧里去掉)。
// 固件运行时不使用 PET_TIPS(PET_FW_BENCHY=0);若以后重新打开 Benchy,挤出线从出料口画到模型顶面,含义一致。
#define PET_FW_BENCHY 0
#define TIP_X      (CANVAS_X + PET_ANIMS[PET_PRINT].ax)
#define TIP_Y      (CANVAS_Y + PET_ANIMS[PET_PRINT].ay)
#define BED_Y      (TIP_Y + BENCHY_H)
#define BENCHY_X   (TIP_X - BENCHY_W / 2)

static const char *TITLES[PET_STATE_COUNT] = {
    "空闲", "打印中", "完成", "暂停", "断料", "失败",
};

static lv_obj_t *s_scr, *s_title, *s_base, *s_hair, *s_shade, *s_info, *s_big, *s_bar, *s_border;
static lv_obj_t *s_strand, *s_data;                 // 挤出的线:当前帧出料口 -> 模型顶面
static lv_point_precise_t s_strand_pts[2];
static lv_obj_t *s_clock, *s_pct, *s_bed, *s_model, *s_fade, *s_hearts[3], *s_page;
static lv_image_dsc_t s_base_dsc[2], s_hair_dsc[2], s_shade_dsc[2];
static int s_pp;                       // ping-pong:每帧换一个描述符,确保 LVGL 重新取图
static lv_timer_t *s_anim_timer, *s_blink_timer;
static pet_state_t s_cur = PET_STATE_COUNT, s_pending = PET_IDLE;
static int s_frame;
static int s_rows = -1;                // Benchy 已显露的行数
static uint32_t s_fil = PET_RECOLOR_FALLBACK_RGB, s_fil_shade = PET_RECOLOR_FALLBACK_RGB, s_model_fil;   // s_fil=#FF00FF; s_fil_shade=#C000C0
static bool s_fading;
static ui_screen_t s_screen = UI_PET;
static bool s_reapply;
static char s_screen_arg[48];
static uint16_t s_canvas_buf[BENCHY_W * BENCHY_H];

static void place_center(lv_obj_t *l, int cy);
static void build_detail(void);
static void build_header(void);
static void build_settings(void);
static void update_detail(pet_state_t st, const printer_status_t *s, link_state_t link);
static void update_header_visibility(void);
static lv_obj_t *s_hdr_dot, *s_hdr_old_dot, *s_fpage;
static bool s_fil_on;

#define CONN_SPR_DY 4   // v4.4.2:「连接中/未连上」页角色下移 4px(对齐设计稿 5_conn_*)
static int s_spr_dy;    // 当前精灵额外 y 偏移
// ---- v4.4.1:只在值真的变了时才改 LVGL 对象 ----
// 真机日志:主循环每 50 ms 无条件重设文字/颜色/字体/位置,每次都让 LVGL 重绘,taskLVGL 占满 CPU,
// 云端任务(bambu_tok)一直拿不到 CPU,永远没连 MQTT。下面这些包装函数值不变就直接返回。
static void ui_txt(lv_obj_t *l, const char *t) {
    const char *o = lv_label_get_text(l);
    if (o && t && !strcmp(o, t)) return;
    lv_label_set_text(l, t);
}
static void ui_txtf(lv_obj_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ui_txtf(lv_obj_t *l, const char *fmt, ...) {
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ui_txt(l, b);
}
static void ui_tcol(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel) {
    if (lv_color_eq(lv_obj_get_style_text_color(o, sel), c)) return;
    lv_obj_set_style_text_color(o, c, sel);
}
static void ui_bgcol(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel) {
    if (lv_color_eq(lv_obj_get_style_bg_color(o, sel), c)) return;
    lv_obj_set_style_bg_color(o, c, sel);
}
static void ui_font(lv_obj_t *o, const lv_font_t *f, lv_style_selector_t sel) {
    if (lv_obj_get_style_text_font(o, sel) == f) return;
    lv_obj_set_style_text_font(o, f, sel);
}
#define UI_TXT ui_txt
#define UI_TXTF ui_txtf
#define UI_TCOL ui_tcol
#define UI_BGCOL ui_bgcol
#define UI_FONT ui_font
static void set_hidden(lv_obj_t *o, bool h) {   // v4.4.1:状态没变就不碰(每次改都会触发重绘)
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == h) return;
    if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *s_set;
static bool s_detail;
static uint16_t c565(uint32_t rgb) {
    return (uint16_t)(((rgb >> 19) & 0x1F) << 11 | ((rgb >> 10) & 0x3F) << 5 | ((rgb >> 3) & 0x1F));
}
static uint32_t shade(uint32_t rgb, int pct) {  // pct>100 变亮
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (pct <= 100) { r = r * pct / 100; g = g * pct / 100; b = b * pct / 100; }
    else { int k = pct - 100; r += (255 - r) * k / 100; g += (255 - g) * k / 100; b += (255 - b) * k / 100; }
    return (uint32_t)(r << 16 | g << 8 | b);
}

static void draw_model(int rows) {
    // 从底往上显露 rows 行;每 3 行一道层纹;最顶一行是亮色"正在挤出"的线。
    uint16_t bg = c565(PET_ANIMS[PET_PRINT].bg);
    uint16_t c_main = c565(s_fil), c_line = c565(shade(s_fil, 78)), c_top = c565(shade(s_fil, 160));
    int first = BENCHY_H - rows;
    for (int y = 0; y < BENCHY_H; y++) {
        for (int x = 0; x < BENCHY_W; x++) {
            bool on = BENCHY_MASK[y * BENCHY_STRIDE + x / 8] & (0x80 >> (x % 8));
            uint16_t c = bg;
            if (on && y >= first) {
                int from_bottom = BENCHY_H - 1 - y;
                c = (y == first) ? c_top : (from_bottom % 3 == 2 ? c_line : c_main);
            }
            s_canvas_buf[y * BENCHY_W + x] = c;
        }
    }
    lv_obj_invalidate(s_model);
}

static int model_top_y(void) {
    // 喷嘴尖贴着当前顶层;还没显露的行里找"有像素"的最高行,空行时贴床面。
    if (s_rows <= 0) return BED_Y;
    return BED_Y - s_rows;   // 0% = 床面, 100% = TIP_Y
}

// 闲着时每 3-6 分钟随机伸个懒腰(idle_stretch 播一遍再回 idle 循环)
#define PET_ANIM_STRETCH PET_STATE_COUNT
static bool s_stretch;
static uint32_t s_stretch_at;
static void schedule_stretch(void) { s_stretch = false; s_stretch_at = lv_tick_get() + (180 + esp_random() % 181) * 1000u; }

static void show_frame(void) {
    const pet_anim_desc_t *a = &PET_ANIMS[s_stretch ? PET_ANIM_STRETCH : s_cur];
    const pet_frame_desc_t *f = &PET_FRAMES[a->first + (s_frame % a->count)];
    s_pp ^= 1;
    lv_image_dsc_t *b = &s_base_dsc[s_pp], *h = &s_hair_dsc[s_pp];
    memset(b, 0, sizeof(*b));
    b->header.magic = LV_IMAGE_HEADER_MAGIC;
    b->header.w = f->w; b->header.h = f->h;
    b->header.cf = a->kind ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    b->header.stride = f->w * 2;
    b->data = pet_frames_bin + f->off;
    b->data_size = (uint32_t)f->w * f->h * (a->kind ? 3 : 2);
    lv_image_set_src(s_base, b);
    int x, y;
    const int16_t *crop = PET_OFFS[a->first + (s_frame % a->count)];
    int cx = 0, cy = 0;   // 画布(150x200)原点;紧裁帧贴在 原点+offset,nozzle_tip 是画布坐标
    if (a->kind) { cx = CANVAS_X + f->dx; cy = CANVAS_Y + f->dy;
                   x = cx + crop[0]; y = cy + crop[1]; }
    else { x = a->ax; y = a->ay; }
    y += s_spr_dy;
    lv_obj_set_pos(s_base, x, y);
    if (PET_FW_BENCHY && a->kind && s_cur == PET_PRINT && !s_stretch) {
        // 精灵按各帧出料口平均值(ax,ay)对准 Benchy;当前帧出料口随热端晃,线从它拉到模型顶面中心
        const int16_t *tip = PET_TIPS[a->first + (s_frame % a->count)];
        int tx = cx + tip[0], ty = cy + tip[1], top = model_top_y();
        if (ty < top - 1) {   // 旧版单点 nozzle_tip_in_sprite:嘴尖贴着模型,不画线
            s_strand_pts[0].x = tx; s_strand_pts[0].y = ty; s_strand_pts[1].x = TIP_X; s_strand_pts[1].y = top;
            lv_line_set_points(s_strand, s_strand_pts, 2);
            lv_obj_set_style_line_color(s_strand, lv_color_hex(s_fil), 0);
            lv_obj_remove_flag(s_strand, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(s_strand, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(s_strand, LV_OBJ_FLAG_HIDDEN);
    if (f->mask_off >= 0) {   // #FF00FF 换色层 -> 耗材色
        memset(h, 0, sizeof(*h));
        h->header.magic = LV_IMAGE_HEADER_MAGIC;
        h->header.w = f->w; h->header.h = f->h; h->header.cf = LV_COLOR_FORMAT_A8; h->header.stride = f->w;
        h->data = pet_frames_bin + f->mask_off; h->data_size = (uint32_t)f->w * f->h;
        lv_image_set_src(s_hair, h);
        lv_obj_set_pos(s_hair, x, y);
        lv_obj_set_style_image_recolor(s_hair, lv_color_hex(s_fil), 0);
        lv_obj_remove_flag(s_hair, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_hair, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_dsc_t *sh = &s_shade_dsc[s_pp];
    if (f->shade_mask_off >= 0) {   // #C000C0 阴影键 -> fil*0.7(近黑提亮);无额外整帧缓冲
        memset(sh, 0, sizeof(*sh));
        sh->header.magic = LV_IMAGE_HEADER_MAGIC;
        sh->header.w = f->w; sh->header.h = f->h; sh->header.cf = LV_COLOR_FORMAT_A8; sh->header.stride = f->w;
        sh->data = pet_frames_bin + f->shade_mask_off; sh->data_size = (uint32_t)f->w * f->h;
        lv_image_set_src(s_shade, sh);
        lv_obj_set_pos(s_shade, x, y);
        lv_obj_set_style_image_recolor(s_shade, lv_color_hex(s_fil_shade), 0);
        lv_obj_remove_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    }
}

static void anim_tick(lv_timer_t *t) {
    (void)t;
    if (s_cur >= PET_STATE_COUNT) return;
    if (s_screen == UI_SETUP_QR) return;   // v4.5:二维码页整屏盖住角色,不播动画(不重绘)
    s_frame++;
    if (s_cur == PET_IDLE && s_screen == UI_PET) {
        if (s_stretch && s_frame >= PET_ANIMS[PET_ANIM_STRETCH].count) { schedule_stretch(); s_frame = 0; }
        else if (!s_stretch && (int32_t)(lv_tick_get() - s_stretch_at) >= 0) { s_stretch = true; s_frame = 0; }
    } else if (s_stretch) { schedule_stretch(); }
    show_frame();
}

static void blink_tick(lv_timer_t *t) {
    (void)t;
    static bool on;
    on = !on;
    if (1) return;   // v4.1:PM 确认去掉断料/失败的彩色边框
    if (s_cur == PET_EMPTY) lv_obj_set_style_border_opa(s_border, on ? LV_OPA_COVER : LV_OPA_20, 0);
    else if (s_cur == PET_FAIL) lv_obj_set_style_border_opa(s_border, on ? LV_OPA_80 : LV_OPA_10, 0);
}

static void apply_state(pet_state_t st) {
    s_cur = st; s_frame = 0; schedule_stretch();
    const pet_anim_desc_t *a = &PET_ANIMS[st];
    UI_BGCOL(s_scr, lv_color_hex(a->bg), 0);
    UI_TXT(s_title, TITLES[st]);
    // B5:空闲/打印中/完成 标题绿色,暂停/断料/失败 白色
    UI_TCOL(s_title, lv_color_hex(st <= PET_DONE ? COL_TITLE : COL_WHITE), 0);
    UI_FONT(s_title, &font_pet_18, 0);
    place_center(s_title, TITLE_Y);
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_border, LV_OBJ_FLAG_HIDDEN);
    bool printing = st == PET_PRINT;
    lv_obj_add_flag(s_bed, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_model, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
    if (PET_FW_BENCHY && printing) { lv_obj_remove_flag(s_bed, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_model, LV_OBJ_FLAG_HIDDEN); }
    if (printing) { lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_big, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_big, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_info, LV_OBJ_FLAG_HIDDEN); }
    lv_timer_set_period(s_anim_timer, a->frame_ms);
    s_rows = -1; s_model_fil = 0;
    show_frame();
#if CONFIG_PET_LOG_REPORT_KEYS
    {   // v4.6:状态切换时打点(换色/大 done 帧是否抬高峰值)
        const pet_frame_desc_t *f = &PET_FRAMES[a->first];
        ESP_LOGI("pet", "heap@mark_state st=%d frame=%dx%d mask=%d free=%u min=%u largest=%u",
                 (int)st, (int)f->w, (int)f->h, f->mask_off >= 0 ? 1 : 0,
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
#endif
}

#ifndef PET_STATE_FADE
#define PET_STATE_FADE 0
#endif
static void fade_opa(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }
static void fade_in_done(lv_anim_t *an);
static void fade_out_done(lv_anim_t *an) {
    (void)an;
    apply_state(s_pending);
    lv_anim_t b; lv_anim_init(&b);
    lv_anim_set_var(&b, s_fade); lv_anim_set_exec_cb(&b, fade_opa);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP); lv_anim_set_duration(&b, 220);
    lv_anim_set_completed_cb(&b, fade_in_done);
    lv_anim_start(&b);
}
static void start_transition(pet_state_t st);
static void fade_in_done(lv_anim_t *an) {
    (void)an;
    s_fading = false;
    lv_obj_add_flag(s_fade, LV_OBJ_FLAG_HIDDEN);
    if (s_pending != s_cur) start_transition(s_pending);  // 过渡期间状态又变了
}
static void start_transition(pet_state_t st) {
    s_pending = st;
    if (s_fading) return;
    // v4.6 headfix: no full-screen black fade on state change (caused flash on demo click);
    // swap pet sprite + text in place.
    if (!PET_STATE_FADE || s_cur >= PET_STATE_COUNT) { apply_state(st); return; }
    s_fading = true;
    lv_obj_move_foreground(s_fade);
    lv_obj_remove_flag(s_fade, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, s_fade); lv_anim_set_exec_cb(&a, fade_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER); lv_anim_set_duration(&a, 180);
    lv_anim_set_completed_cb(&a, fade_out_done);
    lv_anim_start(&a);
}

static void place_center(lv_obj_t *l, int cy) {
    if (lv_obj_get_style_align(l, 0) == LV_ALIGN_CENTER && lv_obj_get_style_x(l, 0) == 0 && lv_obj_get_style_y(l, 0) == cy - 160) return;
    lv_obj_align(l, LV_ALIGN_CENTER, 0, cy - 160);
}
static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int y) {
    lv_obj_t *l = lv_label_create(p);
    UI_FONT(l, f, 0);
    UI_TCOL(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 220);
    UI_TXT(l, "");
    place_center(l, y);
    return l;
}

// ============================ v4.5 配网/绑定/重新扫码/到期/恢复出厂============================
// 坐标照设计稿:文字 y = PIL d.text 的 y(行顶 = 字体 ascender),换算成 LVGL 标签顶:y + asc - (line_height - base_line)。
// 角色:设计稿 petc(state, anchor) = 画布底边 anchor;固件画布底边 CANVAS_Y+200 = 222,差值走 s_spr_dy。
// 这些控件只在 pet_ui_set_screen 换页时设置一次;每 tick 的 setup_tick() 只做"值变了才改"的检查。
#define SU_AMB   0xFFB020
#define SU_RED   0xE65A46
#define SU_GREEN 0x00AE42
#define SU_W     0xF0F0F0
#define SU_GR    0x8C8C8C
#define SU_CARD  0x1C1E20
LV_FONT_DECLARE(font_pet_10);
LV_FONT_DECLARE(font_pet_13);
static lv_obj_t *s_su, *su_title, *su_main, *su_sub, *su_sub2, *su_btn, *su_btn_lbl, *su_card, *su_card_t1, *su_card_t2;
static lv_obj_t *su_ring, *su_ring_lbl, *su_rbar;
static lv_obj_t *s_qr_box, *s_qr, *s_qr_step[3];
static int su_asc(const lv_font_t *f) {   // Noto Sans CJK 在 PIL 里的 ascent(getmetrics()[0])
    if (f == &font_pet_10) return 12;
    if (f == &font_pet_12) return 14;
    if (f == &font_pet_13) return 16;
    if (f == &font_pet_14) return 17;
    if (f == &font_pet_15) return 18;
    if (f == &font_pet_16) return 19;
    return 22;
}
static int su_y(const lv_font_t *f, int pil_y) { return pil_y + su_asc(f) - (f->line_height - f->base_line); }
static void su_pos(lv_obj_t *o, int x, int y) {   // 位置没变就不动(不触发重绘)
    if (lv_obj_get_style_x(o, 0) == x && lv_obj_get_style_y(o, 0) == y && lv_obj_get_style_align(o, 0) == LV_ALIGN_DEFAULT) return;
    lv_obj_set_pos(o, x, y);
}
static lv_obj_t *su_box(lv_obj_t *p, int x0, int y0, int x1, int y1, int r, uint32_t col) {   // PIL 矩形 (x0,y0,x1,y1) 含端点
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x0, y0); lv_obj_set_size(o, x1 - x0 + 1, y1 - y0 + 1);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}
static lv_obj_t *su_lbl(lv_obj_t *p, const lv_font_t *f, uint32_t col, int pil_y) {   // 水平居中(宽 220,x=10)
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 220); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, ""); lv_obj_set_pos(l, 10, su_y(f, pil_y));
    return l;
}
static void su_set(lv_obj_t *l, const lv_font_t *f, uint32_t col, int pil_y, const char *t) {
    UI_FONT(l, f, 0); UI_TCOL(l, lv_color_hex(col), 0); UI_TXT(l, t); su_pos(l, 10, su_y(f, pil_y)); set_hidden(l, false);
}
static void build_setup(void) {
    // 普通页面(绑定/重新扫码/到期/恢复出厂)的文字层:透明全屏容器,盖在角色上面
    s_su = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_su);
    lv_obj_remove_flag(s_su, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_su, 240, 320);
    su_card = su_box(s_su, 14, 38, 226, 112, 12, SU_CARD);            // v4.7.1:整体下移 8px,给顶部状态条让位            // 到期提醒卡片(琥珀色描边 2px)
    lv_obj_set_style_border_color(su_card, lv_color_hex(SU_AMB), 0); lv_obj_set_style_border_width(su_card, 2, 0);
    su_card_t1 = su_lbl(s_su, &font_pet_15, SU_W, 50);
    su_card_t2 = su_lbl(s_su, &font_pet_13, SU_AMB, 78);
    su_title = su_lbl(s_su, &font_pet_15, SU_GREEN, 54);
    su_main = su_lbl(s_su, &font_pet_16, SU_W, 222);
    su_sub = su_lbl(s_su, &font_pet_12, SU_GR, 248);
    su_sub2 = su_lbl(s_su, &font_pet_12, SU_GR, 292);
    su_btn = su_box(s_su, 40, 272, 200, 302, 15, SU_GREEN);           // 「按中键重新扫码」
    su_btn_lbl = su_lbl(s_su, &font_pet_13, 0x000000, 277);
    su_ring = su_box(s_su, 30, 214, 58, 242, LV_RADIUS_CIRCLE, 0);    // 恢复出厂倒计时圈:红色 2px 描边
    lv_obj_set_style_bg_opa(su_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(su_ring, lv_color_hex(SU_RED), 0); lv_obj_set_style_border_width(su_ring, 2, 0);
    su_ring_lbl = lv_label_create(s_su);
    lv_obj_set_style_text_font(su_ring_lbl, &font_pet_14, 0); lv_obj_set_style_text_color(su_ring_lbl, lv_color_hex(SU_RED), 0);
    lv_obj_set_style_text_align(su_ring_lbl, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_width(su_ring_lbl, 28);
    lv_label_set_text(su_ring_lbl, ""); lv_obj_set_pos(su_ring_lbl, 30, su_y(&font_pet_14, 217));
    su_rbar = lv_bar_create(s_su);
    lv_obj_set_pos(su_rbar, 68, 222); lv_obj_set_size(su_rbar, 143, 13);
    lv_obj_set_style_radius(su_rbar, 6, LV_PART_MAIN); lv_obj_set_style_radius(su_rbar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(su_rbar, lv_color_hex(0x343846), LV_PART_MAIN); lv_obj_set_style_bg_opa(su_rbar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(su_rbar, lv_color_hex(SU_RED), LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(su_rbar, 0, 0);
    lv_bar_set_range(su_rbar, 0, 100);
    lv_obj_add_flag(s_su, LV_OBJ_FLAG_HIDDEN);

    // 配网二维码页:黑底整屏盖住角色(设计:不放小角色)
    s_qr_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_qr_box);
    lv_obj_remove_flag(s_qr_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_qr_box, 240, 320);
    lv_obj_set_style_bg_color(s_qr_box, lv_color_black(), 0); lv_obj_set_style_bg_opa(s_qr_box, LV_OPA_COVER, 0);
    lv_obj_t *qt = su_lbl(s_qr_box, &font_pet_15, SU_W, 30);
    lv_label_set_text(qt, "帮我连上 Wi-Fi");
    // 二维码:Passport-XXXX 的 WIFI: 串 = 版本 3(29 模块,ECC M),每模块 3px = 87px;白底 87+16 = 103,顶 y=58,码在 y=66
    su_box(s_qr_box, (240 - 103) / 2, 58, (240 + 103) / 2, 58 + 103, 8, 0xFFFFFF);
    s_qr = lv_qrcode_create(s_qr_box);
    lv_qrcode_set_size(s_qr, 87);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_pos(s_qr, (240 - 87) / 2, 66);
    int y0 = 58 + 103 + 12;
    su_box(s_qr_box, 14, y0, 226, y0 + 70, 10, SU_CARD);
    static const char *const STEPS[3] = { "手机相机扫码", "", "在弹出的网页里填写" };
    static const char *const NUM[3] = { "1", "2", "3" };
    for (int i = 0; i < 3; i++) {
        int yy = y0 + 8 + i * 20;
        su_box(s_qr_box, 24, yy + 2, 38, yy + 16, LV_RADIUS_CIRCLE, SU_GREEN);
        lv_obj_t *n = lv_label_create(s_qr_box);
        lv_obj_set_style_text_font(n, &font_pet_10, 0); lv_obj_set_style_text_color(n, lv_color_black(), 0);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_width(n, 15);
        lv_label_set_text(n, NUM[i]); lv_obj_set_pos(n, 24, su_y(&font_pet_10, yy + 1));
        lv_obj_t *t = lv_label_create(s_qr_box);
        lv_obj_set_style_text_font(t, &font_pet_12, 0); lv_obj_set_style_text_color(t, lv_color_hex(SU_W), 0);
        lv_obj_set_width(t, 226 - 46 - 4); lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_label_set_text(t, STEPS[i]); lv_obj_set_pos(t, 46, su_y(&font_pet_12, yy));
        s_qr_step[i] = t;
    }
    lv_obj_add_flag(s_qr_box, LV_OBJ_FLAG_HIDDEN);
}
static int setup_dy(ui_screen_t sc) {   // 设计稿角色锚点 - 固件画布底边 222
    switch (sc) {
    case UI_BOUND:  return 206 - 222;
    case UI_REAUTH: return 214 - 222;
    case UI_EXPIRY: return 262 - 222;
    case UI_RESET:  return 206 - 222;
    default:        return 0;
    }
}
static void setup_apply(ui_screen_t sc, const char *arg) {   // 只在换页时调用
    bool on = sc != UI_PET && sc != UI_SETUP_QR;
    set_hidden(s_su, !on);
    set_hidden(s_qr_box, sc != UI_SETUP_QR);
    if (sc == UI_SETUP_QR) {
        char q[96], st[64]; pet_ap_qr(arg, q, sizeof q); pet_ap_step(arg, st, sizeof st);   // v4.5.2:与 net.c 的 SSID 同源
        lv_qrcode_update(s_qr, q, strlen(q));
        UI_TXT(s_qr_step[1], st);
        lv_obj_move_foreground(s_qr_box);
        return;
    }
    if (!on) return;
    lv_obj_t *all[] = { su_card, su_card_t1, su_card_t2, su_title, su_main, su_sub, su_sub2, su_btn, su_btn_lbl, su_ring, su_ring_lbl, su_rbar };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) set_hidden(all[i], true);
    switch (sc) {
    case UI_BOUND: {
        char b[64]; snprintf(b, sizeof b, "已连上 %s", arg[0] ? arg : "打印机");
        su_set(su_title, &font_pet_15, SU_GREEN, 54, "绑定成功");
        su_set(su_main, &font_pet_16, SU_W, 222, b);
        su_set(su_sub, &font_pet_12, SU_GR, 248, "以后就交给我啦");
        break; }
    case UI_REAUTH:
        su_set(su_title, &font_pet_15, SU_AMB, 30, "登录过期");
        su_set(su_main, &font_pet_16, SU_W, 224, "要重新扫码啦");
        su_set(su_sub, &font_pet_12, SU_GR, 248, "拓竹登录过期了，Wi-Fi 还在");
        set_hidden(su_btn, false);
        su_set(su_btn_lbl, &font_pet_13, 0x000000, 277, "按中键重新扫码");
        break;
    case UI_EXPIRY: {
        char b[48];
#if CONFIG_PET_TEST_SHORT_EXPIRY
        snprintf(b, sizeof b, "登录还剩 %s 分钟", arg);
#else
        snprintf(b, sizeof b, "登录还剩 %s 天", arg);
#endif
        set_hidden(su_card, false);
        su_set(su_card_t1, &font_pet_15, SU_W, 50, "过几天要重新扫码哦");
        su_set(su_card_t2, &font_pet_13, SU_AMB, 78, b);
        su_set(su_sub2, &font_pet_12, SU_GR, 282, "按任意键收起");
        break; }
    case UI_RESET:
        su_set(su_title, &font_pet_15, SU_RED, 30, "恢复出厂设置？");
        set_hidden(su_ring, false); set_hidden(su_ring_lbl, false); set_hidden(su_rbar, false);
        su_set(su_main, &font_pet_13, SU_W, 252, "按住中键 5 秒");
        su_set(su_sub, &font_pet_12, SU_GR, 274, "会清掉 Wi-Fi 和登录信息");
        su_set(su_sub2, &font_pet_12, SU_GR, 292, "松手取消");
        break;
    default: break;
    }
    lv_obj_move_foreground(s_su);
}
static void setup_tick(void) {   // 每次 pet_ui_update(50 ms):只做检查,值没变什么都不改
    set_hidden(s_title, true); set_hidden(s_info, true); set_hidden(s_data, true);
    set_hidden(s_bar, true); set_hidden(s_big, true); set_hidden(s_clock, true);
    int dy = setup_dy(s_screen);
    if (dy != s_spr_dy) { s_spr_dy = dy; if (s_cur < PET_STATE_COUNT) show_frame(); }
}

void pet_ui_create(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    UI_BGCOL(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    s_bed = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_bed);
    lv_obj_set_size(s_bed, 140, 10); lv_obj_set_pos(s_bed, 40, BED_Y);   // 窄一点,右侧给百分比让位   // BED_Y 来自生成的出料口表
    UI_BGCOL(s_bed, lv_color_hex(0x3C404E), 0);
    lv_obj_set_style_bg_opa(s_bed, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bed, 3, 0);
    lv_obj_set_style_border_side(s_bed, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_bed, 3, 0);
    lv_obj_set_style_border_color(s_bed, lv_color_hex(0x967846), 0);

    s_model = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_model, s_canvas_buf, BENCHY_W, BENCHY_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_model, BENCHY_X, BED_Y - BENCHY_H);

    s_strand = lv_line_create(s_scr);
    lv_obj_set_style_line_width(s_strand, 2, 0);
    lv_obj_set_style_line_rounded(s_strand, true, 0);
    lv_obj_add_flag(s_strand, LV_OBJ_FLAG_HIDDEN);

    s_base = lv_image_create(s_scr);
    s_hair = lv_image_create(s_scr);
    lv_obj_set_style_image_recolor_opa(s_hair, LV_OPA_COVER, 0);
    s_shade = lv_image_create(s_scr);
    lv_obj_set_style_image_recolor_opa(s_shade, LV_OPA_COVER, 0);

    s_title = label(s_scr, &font_pet_18, COL_TEXT, 34);
    s_clock = label(s_scr, &font_pet_36, COL_TEXT, 46);
    s_bar = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar, 150, 6); lv_obj_set_pos(s_bar, 45, BAR_Y);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN); lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    s_pct = lv_label_create(s_scr);
    UI_FONT(s_pct, &font_pet_14, 0);
    UI_TCOL(s_pct, lv_color_hex(COL_MINT), 0);
    lv_obj_set_pos(s_pct, 186, BAR_Y); UI_TXT(s_pct, "");
    UI_BGCOL(s_bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    UI_BGCOL(s_bar, lv_color_hex(COL_MINT), LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    s_big = label(s_scr, &font_pet_24, COL_WHITE, MAIN_Y - 4);     // 打印中:大号百分比
    s_info = label(s_scr, &font_pet_18, COL_WHITE, MAIN_Y);        // 其他状态:大字主信息
    s_data = label(s_scr, &font_pet_14, COL_DATA, DATA_Y);         // 灰色数据行

    s_border = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_border);
    // 内缩 3px、圆角 27,与屏幕 30px 圆角遮罩同心,四角不被裁掉(和美术帧一致)
    lv_obj_set_size(s_border, 240 - 2 * 3, 320 - 2 * 3); lv_obj_center(s_border);
    lv_obj_set_style_radius(s_border, 27, 0);
    lv_obj_set_style_border_width(s_border, 3, 0);
    lv_obj_set_style_border_color(s_border, lv_color_hex(COL_ORANGE), 0);
    lv_obj_add_flag(s_border, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        s_hearts[i] = lv_image_create(s_scr);
        lv_image_set_src(s_hearts[i], &img_heart);
        lv_obj_add_flag(s_hearts[i], LV_OBJ_FLAG_HIDDEN);
    }

    build_detail();

    build_setup();   // v4.5 配网/绑定/重新扫码/到期/恢复出厂

    s_fade = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_fade);
    lv_obj_set_size(s_fade, 240, 320);
    UI_BGCOL(s_fade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_fade, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_fade, LV_OBJ_FLAG_HIDDEN);

#if CONFIG_PET_TEST_SHORT_EXPIRY
    // 内部测试版标记:放在顶层,任何页面(含二维码页)都看得到
    lv_obj_t *tb = lv_label_create(lv_layer_top());
    UI_FONT(tb, &font_pet_14, 0);
    UI_TCOL(tb, lv_color_white(), 0);
    UI_BGCOL(tb, lv_color_hex(0xD02020), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tb, 4, 0);
    lv_obj_set_style_pad_hor(tb, 4, 0);
    UI_TXT(tb, "TEST");
    lv_obj_align(tb, LV_ALIGN_TOP_LEFT, 8, 34);   // v4.7:时间在 y≈14..28,TEST 往下让一点   // 左上角(画布 x>=45 之外),不压角色
#endif
    build_header();
    build_settings();
    s_anim_timer = lv_timer_create(anim_tick, 160, NULL);
    s_blink_timer = lv_timer_create(blink_tick, 450, NULL);
    lv_screen_load(s_scr);
}

static const char *link_text(link_state_t l) {
    switch (l) {
    case LINK_DEMO: return "演示模式";
    case LINK_ONLINE: return "云端已连接";
    case LINK_CONNECTING: return "正在连接";
    case LINK_OFFLINE: return "离线";
    case LINK_AUTH_FAIL: return "要重新扫码啦";
    default: return "未配置";
    }
}

// ---- v4.4.1 没数据时的整页「连接中 / 未连上」(设计 conn.py:5_conn_connecting / 5_conn_failed)----
#define CONN_TITLE_Y 259   // 16px 标题中心线
#define CONN_SUB_Y   282   // 12px 副标题中心线
#define CONN_GREY    0x969696   // 线团画成灰色
#define CONN_RED     0xE65A46
#define CONN_AMBER   0xFFB020   // v4.4.3「读不到打印机数据」(conn.py nodata)
static pet_conn_state_t s_conn = PET_CONN_DATA, s_conn_shown = PET_CONN_DATA;
void pet_ui_set_conn(pet_conn_state_t c) { s_conn = c; }
pet_conn_state_t pet_ui_conn_shown(void) { return s_conn_shown; }
static void conn_dot(void) {
    // 绿点只在收到数据后出现:有数据 = 实心绿点;连接中 = 灰色空心圈;未连上 = 红色空心圈
    static int last = -1;
    int k = s_screen == UI_PET ? (int)s_conn_shown : 0;
    if (k == last) return;
    last = k;
    lv_obj_t *dots[2] = { s_hdr_dot, s_hdr_old_dot };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *d = dots[i];
        if (!d) continue;
        if (k == PET_CONN_DATA) {
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0); lv_obj_set_style_border_width(d, 0, 0);
            lv_obj_set_style_bg_color(d, lv_color_hex(COL_TITLE), 0);
        } else {
            lv_obj_set_style_bg_opa(d, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(d, 1, 0);
            lv_obj_set_style_border_color(d, lv_color_hex(k == PET_CONN_FAILED ? CONN_RED : k == PET_CONN_NODATA ? CONN_AMBER : CONN_GREY), 0);
        }
    }
}
void pet_ui_update(pet_state_t st, const printer_status_t *s, link_state_t link) {
    if (s_screen == UI_BOUND) st = PET_DONE;                         // v4.5 设计:绑定 done / 重新扫码 pause / 恢复出厂 empty / 到期 idle
    else if (s_screen == UI_REAUTH) st = PET_PAUSE;
    else if (s_screen == UI_RESET) st = PET_EMPTY;
    else if (s_screen == UI_SETUP_QR || s_screen == UI_EXPIRY) st = PET_IDLE;
    bool conn = s_screen == UI_PET && s_conn != PET_CONN_DATA;
    if (conn) st = s_conn == PET_CONN_FAILED ? PET_EMPTY : s_conn == PET_CONN_NODATA ? PET_PAUSE : PET_IDLE;     // 连接中 = 打盹帧;读不到数据 = 暂停帧;未连上 = 断料哭哭帧
    pet_conn_state_t eff = s_screen == UI_PET ? s_conn : PET_CONN_DATA;
    if (eff != s_conn_shown) {                  // 进出「连接中/未连上」:直接切,不做淡入淡出
        s_conn_shown = eff;
        s_spr_dy = eff != PET_CONN_DATA ? CONN_SPR_DY : 0;
        if (!s_fading) { s_pending = st; s_reapply = false; apply_state(st); }
    }
    // v4.5.2 换页闪屏修复:换页/换机只要求"重新套用同一状态"(s_reapply),以前也走 start_transition(),
    // 全屏黑色遮罩 180 ms 变黑 + 220 ms 变回 = 每次换页闪一下黑。同一状态现在直接套用,不做淡入淡出;状态真的变了才淡出淡入。
    if (s_reapply && st == s_cur && st == s_pending && !s_fading) { s_reapply = false; apply_state(st); }
    else if (st != s_pending || s_reapply || (s_cur >= PET_STATE_COUNT && !s_fading)) { s_reapply = false; start_transition(st); }
    // #FF00FF -> s_fil; #C000C0 -> s_fil_shade (fil*0.7 / 近黑提亮). empty 没料仍灰.
    if (s_screen == UI_BOUND) { s_fil = SU_GREEN; s_fil_shade = pet_recolor_shade_rgb(SU_GREEN); }
    else if (conn || s_screen != UI_PET) { s_fil = CONN_GREY; s_fil_shade = pet_recolor_shade_rgb(CONN_GREY); }
    else if (st == PET_EMPTY && !s->fil_rgb) { s_fil = PET_EMPTY_NOFIL_RGB; s_fil_shade = pet_recolor_shade_rgb(0); }
    else { s_fil = pet_recolor_rgb(s->fil_rgb); s_fil_shade = pet_recolor_shade_rgb(s->fil_rgb); }
    conn_dot();
    if (conn) {
        bool failed = s_conn == PET_CONN_FAILED, nodata = s_conn == PET_CONN_NODATA;
        UI_TXT(s_title, "");
        set_hidden(s_bar, true); set_hidden(s_big, true); set_hidden(s_info, false); set_hidden(s_data, false);
        UI_TXT(s_info, failed ? "未连上拓竹云" : nodata ? "读不到打印机数据" : "连接中…");
        UI_FONT(s_info, &font_pet_16, 0); UI_TCOL(s_info, lv_color_hex(failed ? CONN_RED : nodata ? CONN_AMBER : COL_WHITE), 0); place_center(s_info, CONN_TITLE_Y);
        UI_TXT(s_data, failed ? "检查 Wi-Fi，长按中键进设置" : nodata ? "已连上拓竹云，正在重试" : "正在连接拓竹云");
        UI_FONT(s_data, &font_pet_12, 0); UI_TCOL(s_data, lv_color_hex(0x8C8C8C), 0); place_center(s_data, CONN_SUB_Y);
        set_hidden(s_page, true); if (s_fpage) set_hidden(s_fpage, true);   // 详情/耗材页也显示这一整页,不显示 0°C 或空槽
        update_header_visibility();
        return;
    }
    if (s_screen != UI_PET) { setup_tick(); update_header_visibility(); return; }   // v4.5:配网类页面的内容在换页时已设好
    if (s_screen == UI_PET) { set_hidden(s_page, !s_detail); if (s_fpage) set_hidden(s_fpage, !s_fil_on); }
    UI_FONT(s_data, &font_pet_14, 0); UI_TCOL(s_data, lv_color_hex(COL_DATA), 0); place_center(s_data, DATA_Y);
    char rem[24];
    if (s->remaining_min >= 0) snprintf(rem, sizeof rem, "%dh %02dm", s->remaining_min / 60, s->remaining_min % 60);
    else snprintf(rem, sizeof rem, "--");
    int pct = s->percent < 0 ? 0 : (s->percent > 100 ? 100 : s->percent);
    int noz = (int)(s->nozzle + 0.5f), bed = (int)(s->bed + 0.5f);
    UI_TCOL(s_info, lv_color_hex(COL_WHITE), 0);
    UI_FONT(s_info, &font_pet_18, 0);
    place_center(s_info, MAIN_Y);
    switch (s_cur) {
    case PET_PRINT:
        if (PET_FW_BENCHY && !s_fading) {
            int rows = BED_Y - pet_model_top_y(pct, BED_Y, TIP_Y);
            if (rows > BENCHY_H) rows = BENCHY_H;
            if (pct > 0 && rows < 1) rows = 1;
            if (rows != s_rows || s_model_fil != s_fil) { s_rows = rows; s_model_fil = s_fil; draw_model(rows); show_frame(); }
        }
        lv_bar_set_value(s_bar, pct, LV_ANIM_ON);
        UI_TXTF(s_big, "%d%%", pct);
        UI_TXTF(s_data, "剩余 %s · 喷嘴 %d°C", rem, noz);
        break;
    case PET_IDLE: {               // v4.2:一行「喷嘴 36°C · 热床 34°C」(双喷嘴取在用的那个;升温中显示 当前→目标)
        char tn[20], tb[20];
        pet_format_temp(s->nozzle, s->nozzle_target, tn, sizeof tn); pet_format_temp(s->bed, s->bed_target, tb, sizeof tb);
        UI_TXT(s_info, "空闲中"); UI_TXTF(s_data, "喷嘴 %s · 热床 %s", tn, tb);
        (void)noz; (void)bed;
        break; }
    case PET_DONE: {               // 有开始时间才显示「用时」;不知道就只留「打印完成」
        UI_TXT(s_info, "打印完成");
        char du[24]; pet_format_duration(s->elapsed_min, du, sizeof du);
        if (s->elapsed_min >= 0) UI_TXTF(s_data, "用时 %s", du); else UI_TXT(s_data, "");
        break; }
    case PET_PAUSE: UI_TXT(s_info, "已暂停");   UI_TXTF(s_data, "剩余 %s · 进度 %d%%", rem, pct); break;
    case PET_EMPTY: UI_TXT(s_info, "耗材用完"); UI_TXT(s_data, "请补料后继续"); break;
    case PET_FAIL:
        UI_TXT(s_info, s->print_error ? pet_error_text(s->print_error) : "打印失败");
        if (s->print_error) UI_TXTF(s_data, "HMS %04X-%04X", (unsigned)(s->print_error >> 16), (unsigned)(s->print_error & 0xFFFF));
        else UI_TXT(s_data, "");
        break;
    default: break;
    }
    (void)link;
    lv_obj_remove_flag(s_data, LV_OBJ_FLAG_HIDDEN);
    if (s_detail) update_detail(st, s, link);   // v4.4.1:详情页没开就不更新(以前每 50 ms 都改一遍隐藏的标签)
    update_header_visibility();
}

static void heart_y(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void heart_opa(void *o, int32_t v) { lv_obj_set_style_image_opa(o, (lv_opa_t)v, 0); }
static void heart_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

void pet_ui_react(void) {
    static const int X[3] = { 62, 108, 154 }, Y[3] = { 120, 100, 120 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *h = s_hearts[i];
        lv_anim_delete(h, NULL);
        lv_obj_move_foreground(h);
        lv_obj_set_pos(h, X[i], Y[i]);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, h);
        lv_anim_set_exec_cb(&a, heart_y); lv_anim_set_values(&a, Y[i], Y[i] - 40);
        lv_anim_set_duration(&a, 900); lv_anim_set_delay(&a, i * 90);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
        lv_anim_set_exec_cb(&a, heart_opa); lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in); lv_anim_set_completed_cb(&a, heart_done);
        lv_anim_start(&a);
    }
}

ui_screen_t pet_ui_screen(void) { return s_screen; }

void pet_ui_set_screen(ui_screen_t sc, const char *arg) {
    s_screen = sc;
    snprintf(s_screen_arg, sizeof s_screen_arg, "%s", arg ? arg : "");
    if (sc != UI_PET) { s_detail = false; set_hidden(s_page, true); set_hidden(s_set, true); pet_ui_show_filament(false, false); }
    else s_spr_dy = s_conn_shown != PET_CONN_DATA ? CONN_SPR_DY : 0;
    if (sc == UI_RESET) pet_ui_set_reset_progress(5, 0);
    setup_apply(sc, s_screen_arg);
    if (sc == UI_PET) { set_hidden(s_title, false); set_hidden(s_info, false); }
    update_header_visibility();
    s_reapply = true;              // 让下一次 update 重新套用状态
}

void pet_ui_set_reset_progress(int secs_left, int pct) {
    UI_TXTF(su_ring_lbl, "%d", secs_left);
    if (lv_bar_get_value(su_rbar) != pct) lv_bar_set_value(su_rbar, pct, LV_ANIM_OFF);
    if (secs_left == 0) UI_TXT(su_main, "正在清除");
}

// ============================ v4.2:机器名头部 / 切换动画 / 详情页 / 设置页 ============================
#define COL_CARD   0x1C1E20
#define COL_DIM    0x464646
#define COL_GREY   0x8C8C8C
static lv_obj_t *s_hdr, *s_hdr_name, *s_hdr_old, *s_hdr_old_name, *s_dots[PET_MAX_PRINTERS], *s_xfade;
static int s_hdr_idx, s_hdr_n = 1;
static char s_hdr_product[16];   // v4.4.1:型号(H2 系列没数据时也显示 L/R 两张喷嘴卡)

// 名字里每个字都在 14px 字库里才用原名
static bool font_has_all(const lv_font_t *f, const char *u) {
    while (*u) {
        uint32_t cp; unsigned char c = (unsigned char)*u;
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int i = 1; i < n; i++) { if (!u[i]) return false; cp = (cp << 6) | ((unsigned char)u[i] & 0x3F); }
        u += n;
        lv_font_glyph_dsc_t g;
        if (cp >= 0x20 && !lv_font_get_glyph_dsc(f, &g, cp, 0)) return false;
    }
    return true;
}
static void display_name(char *out, size_t len, const char *name, const char *product, int idx) {
    if (name && name[0] && font_has_all(&font_pet_14, name)) snprintf(out, len, "%s", name);
    else if (product && product[0]) snprintf(out, len, "%s", product);
    else snprintf(out, len, "打印机 %d", idx + 1);
}

static lv_obj_t *mk_hdr_group(lv_obj_t **name, lv_obj_t **dot) {
    lv_obj_t *g = lv_obj_create(s_hdr);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, 240, 24);
    lv_obj_set_pos(g, 0, HDR_Y - 12);
    *name = lv_label_create(g);
    UI_FONT(*name, &font_pet_14, 0);
    UI_TCOL(*name, lv_color_hex(COL_TEXT), 0);
    lv_label_set_long_mode(*name, LV_LABEL_LONG_DOT);
    UI_TXT(*name, "");
    *dot = lv_obj_create(g);
    lv_obj_remove_style_all(*dot);
    lv_obj_set_size(*dot, 6, 6);
    lv_obj_set_style_radius(*dot, 3, 0);
    lv_obj_set_style_bg_opa(*dot, LV_OPA_COVER, 0);
    return g;
}
static void hdr_fill(lv_obj_t *name, lv_obj_t *dot, const char *txt, bool online) {
    UI_TXT(name, txt);
    lv_obj_set_width(name, LV_SIZE_CONTENT);
    lv_obj_update_layout(name);
    int w = lv_obj_get_width(name);
    // v4.7:左右让给状态条(时间/电池)。名字+绿点放在 [SB_NAME_L, SB_NAME_R] 里,能居中就居中,
    // 放不下居中就往左靠(右边电量字更宽);再长用省略号
    if (w > SB_NAME_R - SB_NAME_L - 12) { w = SB_NAME_R - SB_NAME_L - 12; lv_obj_set_width(name, w); }
    int x = (240 - (w + 12)) / 2;
    if (x + w + 12 > SB_NAME_R) x = SB_NAME_R - (w + 12);
    if (x < SB_NAME_L) x = SB_NAME_L;
    lv_obj_set_pos(name, x, 12 - lv_font_get_line_height(&font_pet_14) / 2);
    lv_obj_set_pos(dot, x + w + 6, 9);
    (void)dot; (void)online;   // v4.4.1:绿点颜色只由 conn_dot() 决定(只有收到数据才是绿点)
}
static void dots_place(void) {
    // 首页:名字下方;详情页:最底部(和效果图一致)
    int y = (s_detail || pet_ui_filament_open()) ? 312 : DOTS_Y;
    int n = s_hdr_n, x0 = 120 - (n - 1) * 5;
    static int last_y = -1, last_n = -1, last_idx = -1, last_vis = -1;   // v4.4.1:没变就不重设(重设会触发重绘)
    int vis = !lv_obj_has_flag(s_hdr, LV_OBJ_FLAG_HIDDEN);
    if (y == last_y && n == last_n && s_hdr_idx == last_idx && vis == last_vis) return;
    last_y = y; last_n = n; last_idx = s_hdr_idx; last_vis = vis;
    for (int i = 0; i < PET_MAX_PRINTERS; i++) {
        bool on = i < n && pet_dots_visible(n);   // v4.4 设计规则:只有 >= 2 台才画小圆点
        set_hidden(s_dots[i], !on);
        if (!on) continue;
        int r = i == s_hdr_idx ? 3 : 2;
        lv_obj_set_size(s_dots[i], 2 * r, 2 * r);
        lv_obj_set_style_radius(s_dots[i], r, 0);
        UI_BGCOL(s_dots[i], lv_color_hex(i == s_hdr_idx ? 0xF0F0F0 : COL_DIM), 0);
        lv_obj_set_pos(s_dots[i], x0 + i * 10 - r, y - r);
    }
}

static void build_header(void) {
    s_xfade = lv_obj_create(s_scr);                // 切换机器时盖住机器名以下的内容做交叉淡入
    lv_obj_remove_style_all(s_xfade);
    lv_obj_set_size(s_xfade, 240, 320 - 48); lv_obj_set_pos(s_xfade, 0, 48);
    UI_BGCOL(s_xfade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_xfade, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_xfade, LV_OBJ_FLAG_HIDDEN);
    s_hdr = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_hdr);
    lv_obj_set_size(s_hdr, 240, 48);
    lv_obj_set_pos(s_hdr, 0, 0);
    lv_obj_add_flag(s_hdr, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(s_hdr, LV_OBJ_FLAG_SCROLLABLE);
    s_hdr_old = mk_hdr_group(&s_hdr_old_name, &s_hdr_old_dot);
    lv_obj_add_flag(s_hdr_old, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *g = mk_hdr_group(&s_hdr_name, &s_hdr_dot);
    (void)g;
    for (int i = 0; i < PET_MAX_PRINTERS; i++) {
        s_dots[i] = lv_obj_create(s_scr);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static lv_obj_t *s_demo_lbl;
static bool hdr_on_top(void) {
    // 机器名/小圆点已经在最上层(自顶向下:圆点 3..0、hdr、xfade;演示角标可以压在它们上面)就不用再挪
    lv_obj_t *want[6] = { s_dots[3], s_dots[2], s_dots[1], s_dots[0], s_hdr, s_xfade };
    int32_t i = (int32_t)lv_obj_get_child_count(s_scr) - 1; int k = 0;
    for (; i >= 0 && k < 6; i--) {
        lv_obj_t *c = lv_obj_get_child(s_scr, i);
        if (c == s_demo_lbl) continue;
        if (c != want[k++]) return false;
    }
    return k == 6;
}
static void update_header_visibility(void) {
    // v4.4.1 根因修复:v4.2 起 pet_ui_update 每 50 ms 都调这里,无条件 lv_obj_move_foreground(s_xfade 240x272 + 机器名 + 圆点),
    // 每次都让 LVGL 把几乎整屏(连同角色图)重画一遍并经 SPI 刷出去,taskLVGL 一直忙(真机日志 IDLE 看门狗每 5 秒一次),
    // 优先级更低的云端任务 bambu_tok 拿不到 CPU,永远没去连 MQTT。现在只有层级真的不对时才挪,圆点没变就不重设。
    bool show = s_screen == UI_PET && !(s_set && !lv_obj_has_flag(s_set, LV_OBJ_FLAG_HIDDEN));
    set_hidden(s_hdr, !show);
    if (show) {
        if (!hdr_on_top()) {
            lv_obj_move_foreground(s_xfade);
            lv_obj_move_foreground(s_hdr);
            for (int i = 0; i < PET_MAX_PRINTERS; i++) lv_obj_move_foreground(s_dots[i]);
        }
        dots_place();
    } else for (int i = 0; i < PET_MAX_PRINTERS; i++) set_hidden(s_dots[i], true);
    // 机器名占了顶部:状态标题位置固定在 TITLE_Y(配网/重新登录等页没有机器名,标题仍在 TITLE_Y,不和机器名冲突)
}

void pet_ui_set_header(const char *name, const char *product, bool online, int idx, int n) {
    snprintf(s_hdr_product, sizeof s_hdr_product, "%s", product ? product : "");
    char d[64]; display_name(d, sizeof d, name, product, idx);
    if (lv_anim_get(s_hdr_old, NULL) == NULL) hdr_fill(s_hdr_name, s_hdr_dot, d, online);
    s_hdr_idx = idx; s_hdr_n = n < 1 ? 1 : n;
    update_header_visibility();
}

static void anim_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void xfade_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void hdr_old_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }
static void xfade_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

void pet_ui_switch_printer(int dir, const char *name, const char *product, bool online, int idx, int n) {
    snprintf(s_hdr_product, sizeof s_hdr_product, "%s", product ? product : "");
    // 旧名字:复制当前文字到 old 组,滑出;新名字从另一侧滑入(260 ms)
    UI_TXT(s_hdr_old_name, lv_label_get_text(s_hdr_name));
    lv_obj_set_pos(s_hdr_old_name, lv_obj_get_x(s_hdr_name), lv_obj_get_y(s_hdr_name));
    lv_obj_set_width(s_hdr_old_name, lv_obj_get_width(s_hdr_name));
    lv_obj_set_pos(s_hdr_old_dot, lv_obj_get_x(s_hdr_dot), lv_obj_get_y(s_hdr_dot));
    UI_BGCOL(s_hdr_old_dot, lv_obj_get_style_bg_color(s_hdr_dot, 0), 0);
    lv_obj_remove_flag(s_hdr_old, LV_OBJ_FLAG_HIDDEN);
    char d[64]; display_name(d, sizeof d, name, product, idx);
    hdr_fill(s_hdr_name, s_hdr_dot, d, online);
    lv_obj_t *g_new = lv_obj_get_parent(s_hdr_name);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, anim_x); lv_anim_set_duration(&a, 260); lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_var(&a, s_hdr_old); lv_anim_set_values(&a, 0, -dir * 240); lv_anim_set_completed_cb(&a, hdr_old_done); lv_anim_start(&a);
    lv_anim_set_completed_cb(&a, NULL);
    lv_anim_set_var(&a, g_new); lv_anim_set_values(&a, dir * 240, 0); lv_anim_start(&a);
    // 内容:盖一层黑色 0 -> 不透明 -> 0(中间换成新机器的数据),整体 300 ms
    lv_obj_remove_flag(s_xfade, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t b; lv_anim_init(&b);
    lv_anim_set_var(&b, s_xfade); lv_anim_set_exec_cb(&b, xfade_opa);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP); lv_anim_set_duration(&b, 300); lv_anim_set_delay(&b, 0);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in); lv_anim_set_completed_cb(&b, xfade_done);
    lv_obj_set_style_bg_opa(s_xfade, LV_OPA_70, 0);
    lv_anim_start(&b);
    s_hdr_idx = idx; s_hdr_n = n < 1 ? 1 : n;
    s_reapply = true;
    update_header_visibility();
}

// ---- 详情页 ----
typedef struct { lv_obj_t *box, *icon, *lbl, *val, *tgt; } card_t;   // v4.4.1:val = 当前值(15px),tgt = 「/目标°C」(12px 灰)
static lv_obj_t *s_d_task, *s_d_pct, *s_d_word, *s_d_bar, *s_d_layer, *s_d_rem, *s_d_fil, *s_d_sw, *s_d_fname, *s_d_fsub, *s_d_hint;
static card_t s_cards[4];
static bool s_hint_on;

static lv_obj_t *txt(lv_obj_t *p, const lv_font_t *f, uint32_t col, int x, int y) {
    lv_obj_t *l = lv_label_create(p);
    UI_FONT(l, f, 0);
    UI_TCOL(l, lv_color_hex(col), 0);
    UI_TXT(l, "");
    lv_obj_set_pos(l, x, y);
    return l;
}
static lv_obj_t *rbox(lv_obj_t *p, int x, int y, int w, int h, int r, uint32_t col) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    UI_BGCOL(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
// 详情页坐标(240x320,和 2_detail.png 对齐;tools/pet/preview_v42.py 镜像同一组数字)
#define D_X      14
#define D_W      212
// v4.7.1:详情页整体下移 8px(提示 +4),顶部状态条(时间/电池)和机器名那一行留给状态条
#define D_TASK_Y 46
#define D_PCT_Y  62
#define D_BAR_Y  96
#define D_LAY_Y  105
#define D_CARD_Y 128
#define D_CARD_H 44
#define D_CARD_W 103
#define D_ICO_X  (D_CARD_W - 21)   // v4.4.1 设计 detail_v441:图标 x+82,y+6(16x18 图标右边留 5px)
#define D_ICO_Y  6
#define D_VAL_X  10        // 当前值离卡片左边 10px
#define D_VAL_Y  20        // 15px 当前值顶边
#define D_TGT_Y  23        // 12px「/目标°C」顶边(和设计稿一样下沉 3px)
#define D_FIL_Y  230
#define D_HINT_Y 288
static void build_detail(void) {
    s_page = rbox(s_scr, 0, 0, 240, 320, 0, 0x000000);
    s_d_task = txt(s_page, &font_pet_14, COL_TEXT, D_X, D_TASK_Y);
    lv_label_set_long_mode(s_d_task, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_d_task, D_W);
    lv_obj_set_height(s_d_task, lv_font_get_line_height(&font_pet_14));   // v4.5.2-r3:定高一行,LONG_DOT 才会截断;否则长任务名换行压到百分比
    s_d_pct = txt(s_page, &font_pet_24, COL_TEXT, D_X, D_PCT_Y);
    s_d_word = txt(s_page, &font_pet_14, COL_TITLE, 0, D_PCT_Y + 10);
    lv_obj_set_width(s_d_word, 100); lv_obj_set_x(s_d_word, D_X + D_W - 100);
    lv_obj_set_style_text_align(s_d_word, LV_TEXT_ALIGN_RIGHT, 0);
    s_d_bar = lv_bar_create(s_page);
    lv_obj_set_pos(s_d_bar, D_X, D_BAR_Y); lv_obj_set_size(s_d_bar, D_W, 6);
    lv_obj_set_style_radius(s_d_bar, 3, LV_PART_MAIN); lv_obj_set_style_radius(s_d_bar, 3, LV_PART_INDICATOR);
    UI_BGCOL(s_d_bar, lv_color_hex(COL_DIM), LV_PART_MAIN);
    UI_BGCOL(s_d_bar, lv_color_hex(COL_TITLE), LV_PART_INDICATOR);
    lv_bar_set_range(s_d_bar, 0, 100);
    s_d_layer = txt(s_page, &font_pet_12, COL_GREY, D_X, D_LAY_Y);
    s_d_rem = txt(s_page, &font_pet_12, COL_GREY, 0, D_LAY_Y);
    lv_obj_set_width(s_d_rem, 120); lv_obj_set_x(s_d_rem, D_X + D_W - 120);
    lv_obj_set_style_text_align(s_d_rem, LV_TEXT_ALIGN_RIGHT, 0);
    static const char *LBL[4] = { "L 喷嘴", "R 喷嘴", "热床", "仓温" };
    const lv_image_dsc_t *ICO[4] = { &img_ico_nozzle, &img_ico_nozzle, &img_ico_bed, &img_ico_chamber };
    for (int i = 0; i < 4; i++) {
        card_t *c = &s_cards[i];
        int x = D_X + (i % 2) * (D_CARD_W + 6), y = D_CARD_Y + (i / 2) * (D_CARD_H + 6);
        c->box = rbox(s_page, x, y, D_CARD_W, D_CARD_H, 8, COL_CARD);
        c->icon = lv_image_create(c->box);
        lv_image_set_src(c->icon, ICO[i]);
        lv_obj_set_style_image_recolor(c->icon, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(c->icon, LV_OPA_COVER, 0);
        lv_obj_set_pos(c->icon, D_ICO_X, D_ICO_Y);      // v4.4.1:图标移到右上角(标题行)
        c->lbl = txt(c->box, &font_pet_12, COL_GREY, D_VAL_X, 3);
        UI_TXT(c->lbl, LBL[i]);
        c->val = txt(c->box, &font_pet_15, COL_TEXT, D_VAL_X, D_VAL_Y);
        c->tgt = txt(c->box, &font_pet_12, COL_GREY, D_VAL_X, D_TGT_Y);
    }
    s_d_fil = rbox(s_page, D_X, D_FIL_Y, D_W, 42, 8, COL_CARD);
    s_d_sw = rbox(s_d_fil, 10, 9, 22, 24, 5, 0xF5F5F5);
    lv_obj_set_style_border_color(s_d_sw, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_border_width(s_d_sw, 1, 0);
    s_d_fname = txt(s_d_fil, &font_pet_14, COL_TEXT, 42, 3);
    s_d_fsub = txt(s_d_fil, &font_pet_12, COL_GREY, 42, 23);
    s_d_hint = txt(s_page, &font_pet_12, COL_DIM, 0, D_HINT_Y);
    lv_obj_set_width(s_d_hint, 240);
    lv_obj_set_style_text_align(s_d_hint, LV_TEXT_ALIGN_CENTER, 0);
    UI_TXT(s_d_hint, "上下键切页 · 双击中键换机器");
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

static void temp_card(int i, bool valid, float cur, float target, bool has_target) {
    char c[12], t[16];
    pet_temp_pair(valid, cur, target, has_target, c, sizeof c, t, sizeof t);
    UI_TXT(s_cards[i].val, c); UI_TXT(s_cards[i].tgt, t);
    lv_point_t sz; lv_text_get_size(&sz, c, &font_pet_15, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int x = D_VAL_X + sz.x + 1;
    if (lv_obj_get_x(s_cards[i].tgt) != x) lv_obj_set_x(s_cards[i].tgt, x);
}
static void update_detail(pet_state_t st, const printer_status_t *s, link_state_t link) {
    if (lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) return;
    char b[48];
    UI_TXT(s_d_task, s->subtask[0] ? s->subtask : (s->valid ? "没有打印任务" : link_text(link)));
    int pct = s->percent < 0 ? 0 : (s->percent > 100 ? 100 : s->percent);
    UI_TXTF(s_d_pct, "%d%%", pct);
    lv_bar_set_value(s_d_bar, pct, LV_ANIM_OFF);
    const char *w = s->valid ? pet_status_word(s, st) : link_text(link);
    UI_TXT(s_d_word, w);
    uint32_t wc = (st == PET_FAIL || st == PET_EMPTY) ? 0xFF6A4A : st == PET_PAUSE ? 0xFFB43C : COL_TITLE;
    UI_TCOL(s_d_word, lv_color_hex(s->valid ? wc : COL_GREY), 0);
    UI_BGCOL(s_d_bar, lv_color_hex(s->valid ? wc : COL_DIM), LV_PART_INDICATOR);
    if (s->layer >= 0 && s->total_layer > 0) UI_TXTF(s_d_layer, "层 %d/%d", s->layer, s->total_layer);
    else UI_TXT(s_d_layer, "层 --");
    // 剩余时间:没有数据(或已完成)整行隐藏
    bool rem = s->remaining_min > 0 && (st == PET_PRINT || st == PET_PAUSE || st == PET_EMPTY);
    set_hidden(s_d_rem, !rem);
    if (rem) UI_TXTF(s_d_rem, "剩余 %dh %02dm", s->remaining_min / 60, s->remaining_min % 60);
    // 温度卡片(v4.4.1):「当前/目标°C」,空闲目标 0 也显示;H2 系列没数据时也是 L/R 两张卡
    bool dual = pet_is_dual(s->n_nozzles, s_hdr_product);
    UI_TXT(s_cards[0].lbl, dual ? "L 喷嘴" : "喷嘴");
    set_hidden(s_cards[1].box, !dual);
    bool v = s->valid;
    temp_card(0, v, s->n_nozzles ? s->noz[0] : s->nozzle, s->n_nozzles ? s->noz_t[0] : s->nozzle_target, true);
    if (dual) temp_card(1, v && s->n_nozzles >= 2, s->noz[1], s->noz_t[1], true);
    for (int i = 0; i < 2; i++)    // 在用喷嘴的标签用绿色
        UI_TCOL(s_cards[i].lbl, lv_color_hex(dual && s->active_nozzle == i ? COL_TITLE : COL_GREY), 0);
    temp_card(2, v, s->bed, s->bed_target, true);
    temp_card(3, v && s->has_chamber, s->chamber, s->chamber_target, s->chamber_has_target);
    // 耗材卡:有 fil_type/fil_rgb 才显示;没料时整卡隐藏(不写「未加载」,温度卡与底栏坐标不变)
    bool has_fil = s->fil_type[0] || s->fil_rgb;
    set_hidden(s_d_fil, !has_fil);
    if (has_fil) {
        const char *cn = pet_color_name(s->fil_rgb);
        snprintf(b, sizeof b, "%s%s%s", s->fil_type[0] ? s->fil_type : "耗材", cn[0] ? " · " : "", cn);
        UI_TXT(s_d_fname, b);
        UI_BGCOL(s_d_sw, lv_color_hex(s->fil_rgb ? s->fil_rgb : PET_RECOLOR_FALLBACK_RGB), 0);
        if (s->fil_slot[0] && s->fil_humidity >= 0) UI_TXTF(s_d_fsub, "%s 湿度 %d%%", s->fil_slot, s->fil_humidity);
        else UI_TXT(s_d_fsub, s->fil_slot);
    }
    set_hidden(s_d_hint, !s_hint_on);
}

void pet_ui_show_detail(bool on, bool show_hint) {
    s_detail = on; s_hint_on = show_hint;
    set_hidden(s_page, !on);
    if (on) lv_obj_move_foreground(s_page);
    s_reapply = true;
    update_header_visibility();
}
bool pet_ui_detail_open(void) { return s_detail; }

// ---- 设置页(长按中键)----
static lv_obj_t *s_set_rows[SET_COUNT], *s_set_lbl[SET_COUNT];
static void build_settings(void) {
    s_set = rbox(s_scr, 0, 0, 240, 320, 0, 0x000000);
    lv_obj_t *t = txt(s_set, &font_pet_18, COL_TITLE, 0, 24);
    lv_obj_set_width(t, 240); lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    UI_TXT(t, "设置");
    for (int i = 0; i < SET_COUNT; i++) {
        s_set_rows[i] = rbox(s_set, 20, 70 + i * 50, 200, 40, 10, COL_CARD);
        s_set_lbl[i] = txt(s_set_rows[i], &font_pet_18, COL_TEXT, 14, 8);
    }
    lv_obj_t *h = txt(s_set, &font_pet_12, COL_GREY, 0, 284);
    lv_obj_set_width(h, 240); lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    UI_TXT(h, "上下键选择 · 中键确定");
    lv_obj_add_flag(s_set, LV_OBJ_FLAG_HIDDEN);
}
void pet_ui_show_settings(bool on, int sel, bool demo_on) {
    set_hidden(s_set, !on);
    if (on) {
        lv_obj_move_foreground(s_set);
        UI_TXT(s_set_lbl[SET_DEMO], demo_on ? "演示模式：开" : "演示模式：关");
        UI_TXT(s_set_lbl[SET_REPAIR], "重新配网");
        UI_TXT(s_set_lbl[SET_RESET], "恢复出厂设置");   // v4.5.3:单击进确认页,不再在这里按住
        UI_TXT(s_set_lbl[SET_BACK], "返回");
        for (int i = 0; i < SET_COUNT; i++) {
            bool rst = i == SET_RESET;   // v4.5.3:红字;选中时暗红底(不用绿色高亮,和其它项区分开)
            UI_BGCOL(s_set_rows[i], lv_color_hex(i == sel ? (rst ? 0x5A1E16 : COL_TITLE) : COL_CARD), 0);
            UI_TCOL(s_set_lbl[i], lv_color_hex(rst ? 0xE65A46 : COL_TEXT), 0);
        }
    }
    update_header_visibility();
}
bool pet_ui_settings_open(void) { return s_set && !lv_obj_has_flag(s_set, LV_OBJ_FLAG_HIDDEN); }

// ---- v4.2.1 演示模式角标「n/6」(右上,机器名下方;不压角色:角色 y>=92)----
void pet_ui_set_demo_step(int step) {
    if (!s_demo_lbl) {
        s_demo_lbl = lv_label_create(s_scr);
        UI_FONT(s_demo_lbl, &font_pet_12, 0);
        UI_TCOL(s_demo_lbl, lv_color_hex(COL_GREY), 0);
        lv_obj_set_style_text_align(s_demo_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(s_demo_lbl, 60);
        lv_obj_set_pos(s_demo_lbl, DEMO_X, DEMO_Y);
    }
    bool show = step >= 0 && s_screen == UI_PET && !s_detail && !pet_ui_filament_open() && !pet_ui_settings_open();
    show = show && pet_demo_badge_on_page(pet_ui_filament_open() ? 2 : s_detail ? 1 : 0);   // v4.4:角标只在首页,详情/耗材页靠底部提示里的 n/6
    set_hidden(s_demo_lbl, !show);
    if (show) {
        UI_TXTF(s_demo_lbl, "%d/6", step + 1);
        if (lv_obj_get_index(s_demo_lbl) != (int32_t)lv_obj_get_child_count(s_scr) - 1) lv_obj_move_foreground(s_demo_lbl);   // v4.4.1:已在最上层就不挪
    }
}

// ---- v4.3 耗材页----
// 每行是一个圆角卡片对象,卡片里的名字、湿度、料盘、标签都在 DRAW_MAIN_END 里直接画,
// 不为每个料槽建控件:4 个行对象就够(一屏最多 3 台 AMS + 外挂行),LVGL 池子只多约 1.5 KB。
#define F_TITLE_Y  49          // 绿色「耗材」(14px)
#define F_ROW_Y0   70          // 第一行卡片顶边;可视区 70..284(PET_AMS_VIEW_H)
#define F_HINT_Y   290         // 底部提示(12px)
#define F_SLOT_X0  80          // 第 1 个料槽中心(相对卡片;绝对 x = 94,和 4_ams_v4.3.png 一致)
#define F_SLOT_DX  33
#define F_DISK_CY  22          // 料盘中心(相对卡片顶)
#define F_DISK_R   11
#define F_HUB_R    7           // 白色轮毂
#define F_HOLE_R   4           // 中心孔;打印中时孔里的 3 根辐条每 2 秒转一圈
#define F_FRAME_T  7           // 在用绿框:相对卡片 y 7..51,x cx-16..cx+16,2px,圆角 6
#define F_FRAME_B  51
#define F_TAG_T    2           // L/R 标签顶边离卡片顶 2px;字变大只往下长(F_TAG_H),绝不往上
#define F_TAG_W    10          // 10x9 绿色圆角块(两个喷嘴同槽 "LR" 时横向加宽)
#define F_TAG_H    (lv_font_get_line_height(&font_pet_8) > 9 ? lv_font_get_line_height(&font_pet_8) : 9)
#define F_TYPE_Y   36          // 材料文字顶边(12px)
#define F_SPIN_MS  2000
#define F_ROWS     4
#define COL_LOW    0xFF4A3A
#define COL_DROP   0x4AA8FF
typedef struct { char name[8], hum[8], type[4][8]; } fil_txt_t;
static pet_ams_t s_fam;                // 当前显示的数据(UI 自己一份,绘制回调在 LVGL 刷新时读)
static fil_txt_t s_fil_txt[PET_AMS_MAX_UNITS];   // 预先排好的文字(名字/湿度/截断后的材料名)
static char s_ext_txt[PET_AMS_MAX_EXT][16];
static lv_obj_t *s_frow[F_ROWS], *s_fhint, *s_fempty;
static int s_frow_of[F_ROWS];          // 每个行对象当前画第几行(-1 = 隐藏)
static int s_ffirst;
static bool s_fil_hint, s_fspin;
static lv_timer_t *s_fspin_tmr;
// 打印中:每 125 ms 只重画在用料槽的中心孔(9x9 像素),16 步一圈 = 2 秒
static void fspin_tick(lv_timer_t *tm) {
    (void)tm;
    if (!s_fil_on || !s_fspin) return;
    for (int k = 0; k < F_ROWS; k++) {
        int row = s_frow_of[k];
        if (row < 0 || row >= s_fam.n_units) continue;
        lv_area_t c; lv_obj_get_coords(s_frow[k], &c);
        const pet_ams_unit_t *u = &s_fam.u[row];
        for (int sl = 0; sl < u->nslot; sl++) {
            if (!(s_fam.slot[u->first + sl].flags & PET_TRAY_ACTIVE)) continue;
            int cx = c.x1 + F_SLOT_X0 + sl * F_SLOT_DX, cy = c.y1 + F_DISK_CY;
            lv_area_t a = { cx - F_HOLE_R, cy - F_HOLE_R, cx + F_HOLE_R, cy + F_HOLE_R };
            lv_obj_invalidate_area(s_frow[k], &a);
        }
    }
}
void pet_ui_filament_set_spinning(bool on) {
    if (on == s_fspin) return;
    s_fspin = on;
    if (!s_fspin_tmr) s_fspin_tmr = lv_timer_create(fspin_tick, F_SPIN_MS / 16, NULL);
    if (on) lv_timer_resume(s_fspin_tmr); else lv_timer_pause(s_fspin_tmr);
    for (int k = 0; k < F_ROWS; k++) if (s_frow[k]) lv_obj_invalidate(s_frow[k]);   // 停下时回到静止角度
}

static int text_w(const char *t, size_t n, const lv_font_t *f) {
    char b[32]; if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, t, n); b[n] = 0;
    lv_point_t sz; lv_text_get_size(&sz, b, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}
static void fit_text(char *dst, size_t len, const char *src, const lv_font_t *f, int max_w) {
    snprintf(dst, len, "%s", src);
    size_t n = strlen(dst);
    while (n > 1 && text_w(dst, n, f) > max_w) dst[--n] = 0;
}
static void draw_disk(lv_layer_t *L, int cx, int cy, const pet_tray_t *t, int r, int spin_deg) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { cx - r, cy - r, cx + r - 1, cy + r - 1 };
    if (!t || (t->flags & PET_TRAY_EMPTY) || !(t->flags & PET_TRAY_KNOWN)) {
        // 空槽:灰色虚线圈(12 段弧)
        lv_draw_arc_dsc_t c; lv_draw_arc_dsc_init(&c);
        c.center.x = cx; c.center.y = cy; c.radius = r - 1; c.width = 1; c.color = lv_color_hex(COL_GREY);
        for (int k = 0; k < 12; k++) { c.start_angle = k * 30; c.end_angle = k * 30 + 16; lv_draw_arc(L, &c); }
        return;
    }
    d.bg_color = lv_color_make(t->r, t->g, t->b); d.bg_opa = LV_OPA_COVER;
    if (pet_tray_dark(t)) { d.border_color = lv_color_hex(0x6E6E6E); d.border_width = 1; d.border_opa = LV_OPA_COVER; }
    lv_draw_rect(L, &d, &a);
    // 白色轮毂(半径 7/11)+ 中心孔:深色圆 + 3 根浅色辐条;spin_deg 由调用方给(打印中随时间转,否则 0)
    lv_draw_rect_dsc_init(&d); d.radius = LV_RADIUS_CIRCLE; d.bg_opa = LV_OPA_COVER;
    int wr = (r * F_HUB_R + F_DISK_R / 2) / F_DISK_R;
    d.bg_color = lv_color_hex(0xF0F0F0);
    lv_area_t w = { cx - wr, cy - wr, cx + wr - 1, cy + wr - 1 }; lv_draw_rect(L, &d, &w);
    int hr = r >= F_DISK_R ? F_HOLE_R : (r * F_HOLE_R + F_DISK_R / 2) / F_DISK_R;
    d.bg_color = lv_color_hex(0x282828);
    lv_area_t h = { cx - hr, cy - hr, cx + hr - 1, cy + hr - 1 }; lv_draw_rect(L, &d, &h);
    if (r >= F_DISK_R) {
        lv_draw_line_dsc_t ln; lv_draw_line_dsc_init(&ln);
        ln.color = lv_color_hex(0xA0A0A0); ln.width = 1; ln.opa = LV_OPA_COVER;
        for (int k = 0; k < 3; k++) {
            int32_t ang = (spin_deg + k * 120) % 360;
            ln.p1.x = cx; ln.p1.y = cy;
            ln.p2.x = cx + (lv_trigo_cos(ang) * (hr - 1) + (1 << 14)) / 32768;
            ln.p2.y = cy + (lv_trigo_sin(ang) * (hr - 1) + (1 << 14)) / 32768;
            lv_draw_line(L, &ln);
        }
    }
}
static void draw_text_at(lv_layer_t *L, const lv_font_t *f, uint32_t col, const char *t, int x, int y, int w, lv_text_align_t al) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = f; d.color = lv_color_hex(col); d.text = t; d.align = al;
    lv_area_t a = { x, y, x + w - 1, y + lv_font_get_line_height(f) - 1 };
    lv_draw_label(L, &d, &a);
}
static void draw_drop(lv_layer_t *L, int x, int y) {      // 水滴 8x10,左上角 (x,y)
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE; d.bg_color = lv_color_hex(COL_DROP); d.bg_opa = LV_OPA_COVER;
    lv_area_t a = { x, y + 3, x + 7, y + 10 }; lv_draw_rect(L, &d, &a);
    lv_draw_triangle_dsc_t t; lv_draw_triangle_dsc_init(&t);
    t.color = lv_color_hex(COL_DROP); t.opa = LV_OPA_COVER;
    t.p[0].x = x + 4; t.p[0].y = y; t.p[1].x = x; t.p[1].y = y + 6; t.p[2].x = x + 8; t.p[2].y = y + 6;
    lv_draw_triangle(L, &t);
}
static void frow_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target(e);
    int k = (int)(intptr_t)lv_event_get_user_data(e), row = s_frow_of[k];
    if (row < 0) return;
    lv_layer_t *L = lv_event_get_layer(e);
    lv_area_t c; lv_obj_get_coords(o, &c);
    int x = c.x1, y = c.y1;
    if (row < s_fam.n_units) {
        const pet_ams_unit_t *u = &s_fam.u[row];
        const fil_txt_t *tx = &s_fil_txt[row];
        draw_text_at(L, &font_pet_14, COL_TEXT, tx->name, x + 8, y + 9, 64, LV_TEXT_ALIGN_LEFT);
        if (tx->hum[0]) {
            draw_drop(L, x + 8, y + 31);
            draw_text_at(L, &font_pet_12, COL_GREY, tx->hum, x + 19, y + 28, 50, LV_TEXT_ALIGN_LEFT);
        }
        for (int s = 0; s < u->nslot; s++) {
            const pet_tray_t *t = &s_fam.slot[u->first + s];
            int cx = x + F_SLOT_X0 + s * F_SLOT_DX;
            bool act = t->flags & PET_TRAY_ACTIVE;
            if (act) {
                lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
                d.radius = 6; d.bg_opa = LV_OPA_TRANSP; d.border_color = lv_color_hex(COL_TITLE); d.border_width = 2; d.border_opa = LV_OPA_COVER;
                lv_area_t a = { cx - 16, y + F_FRAME_T, cx + 16, y + F_FRAME_B }; lv_draw_rect(L, &d, &a);
            }
            int spin = (act && s_fspin) ? (int)((lv_tick_get() % F_SPIN_MS) * 360 / F_SPIN_MS) : 0;
            draw_disk(L, cx, y + F_DISK_CY, t, F_DISK_R, spin);
            if (pet_tray_low(t)) {
                lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
                d.radius = LV_RADIUS_CIRCLE; d.bg_color = lv_color_hex(COL_LOW); d.bg_opa = LV_OPA_COVER;
                lv_area_t a = { cx + 7, y + F_DISK_CY - 13, cx + 12, y + F_DISK_CY - 8 }; lv_draw_rect(L, &d, &a);
            }
            const char *tag = pet_tray_tag(t);
            if (act && tag[0]) {   // 绿底黑字 L/R 小标签,压在绿框上边;顶边固定在卡片顶下 2px;单喷嘴(tag 为空)只有绿框
                int tw = text_w(tag, strlen(tag), &font_pet_8), th = F_TAG_H;
                int w = tw + 4 > F_TAG_W ? tw + 4 : F_TAG_W, x0 = cx - w / 2;
                lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
                d.radius = 3; d.bg_color = lv_color_hex(COL_TITLE); d.bg_opa = LV_OPA_COVER;
                lv_area_t a = { x0, y + F_TAG_T, x0 + w - 1, y + F_TAG_T + th - 1 };
                lv_draw_rect(L, &d, &a);
                int lh = lv_font_get_line_height(&font_pet_8);
                draw_text_at(L, &font_pet_8, 0x000000, tag, x0 + (w - tw + 1) / 2, y + F_TAG_T + (th - lh) / 2, tw + 2, LV_TEXT_ALIGN_LEFT);
            }
            bool empty = (t->flags & PET_TRAY_EMPTY) || !(t->flags & PET_TRAY_KNOWN);
            draw_text_at(L, &font_pet_12, empty ? COL_GREY : COL_GREY, empty ? "空" : tx->type[s], cx - 16, y + F_TYPE_Y, 32, LV_TEXT_ALIGN_CENTER);
        }
    } else {
        draw_text_at(L, &font_pet_14, COL_TEXT, "外挂料盘", x + 8, y + 4, 80, LV_TEXT_ALIGN_LEFT);
        // 右侧从右往左排:最后一个(右)在最右
        int rx = x + D_W - 8;
        for (int i = s_fam.n_ext - 1; i >= 0; i--) {
            const pet_tray_t *t = &s_fam.ext[i];
            bool set = (t->flags & PET_TRAY_KNOWN) && !(t->flags & PET_TRAY_EMPTY);
            int w = text_w(s_ext_txt[i], strlen(s_ext_txt[i]), &font_pet_12);
            uint32_t col = (t->flags & PET_TRAY_ACTIVE) ? COL_TITLE : COL_GREY;
            draw_text_at(L, &font_pet_12, col, s_ext_txt[i], rx - w, y + 6, w + 2, LV_TEXT_ALIGN_LEFT);
            rx -= w;
            if (set) { rx -= 10; draw_disk(L, rx + 3, y + PET_AMS_EXT_H / 2, t, 6, 0); rx -= 6; }
            const char *side = (t->flags & PET_TRAY_SIDE_L) ? "L" : (t->flags & PET_TRAY_SIDE_R) ? "R" : "";
            if (side[0]) {
                int sw = text_w(side, strlen(side), &font_pet_12);
                rx -= 4 + sw;
                draw_text_at(L, &font_pet_12, col, side, rx, y + 6, sw + 2, LV_TEXT_ALIGN_LEFT);
            }
            rx -= 14;
        }
    }
}
static void fil_layout(void) {
    int n = pet_ams_row_count(&s_fam);
    if (s_ffirst > 0 && s_ffirst >= n) s_ffirst = n ? n - 1 : 0;
    // 往回收:后面空出来时尽量把上面的行拉回来
    while (s_ffirst > 0 && pet_ams_rows_fit(&s_fam, s_ffirst - 1, PET_AMS_VIEW_H) >= n - (s_ffirst - 1)) s_ffirst--;
    int fit = pet_ams_rows_fit(&s_fam, s_ffirst, PET_AMS_VIEW_H), y = F_ROW_Y0;
    for (int k = 0; k < F_ROWS; k++) {
        int row = k < fit ? s_ffirst + k : -1;
        s_frow_of[k] = row;
        set_hidden(s_frow[k], row < 0);
        if (row < 0) continue;
        int h = pet_ams_row_h(&s_fam, row);
        lv_obj_set_pos(s_frow[k], D_X, y); lv_obj_set_size(s_frow[k], D_W, h);
        lv_obj_invalidate(s_frow[k]);
        y += h + PET_AMS_GAP;
    }
    set_hidden(s_fempty, n > 0);
}
static void build_filament(void) {
    s_fpage = rbox(s_scr, 0, 0, 240, 320, 0, 0x000000);
    lv_obj_t *t = txt(s_fpage, &font_pet_14, COL_TITLE, 0, F_TITLE_Y);
    lv_obj_set_width(t, 240); lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    UI_TXT(t, "耗材");
    for (int k = 0; k < F_ROWS; k++) {
        s_frow[k] = rbox(s_fpage, D_X, F_ROW_Y0, D_W, PET_AMS_ROW_H, 8, COL_CARD);
        lv_obj_add_event_cb(s_frow[k], frow_draw, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)k);
        s_frow_of[k] = -1;
        lv_obj_add_flag(s_frow[k], LV_OBJ_FLAG_HIDDEN);
    }
    s_fempty = txt(s_fpage, &font_pet_14, COL_GREY, 0, 150);
    lv_obj_set_width(s_fempty, 240); lv_obj_set_style_text_align(s_fempty, LV_TEXT_ALIGN_CENTER, 0);
    UI_TXT(s_fempty, "还没有收到耗材数据");
    s_fhint = txt(s_fpage, &font_pet_12, COL_DIM, 0, F_HINT_Y);
    lv_obj_set_width(s_fhint, 240); lv_obj_set_style_text_align(s_fhint, LV_TEXT_ALIGN_CENTER, 0);
    UI_TXT(s_fhint, "上下键翻页/切页 · 双击中键换机器");
    lv_obj_add_flag(s_fpage, LV_OBJ_FLAG_HIDDEN);
    pet_ams_reset(&s_fam);
}
void pet_ui_set_filament(const pet_ams_t *a) {
    if (!s_fpage) build_filament();
    if (!memcmp(a, &s_fam, sizeof *a)) return;
    s_fam = *a;
    for (int u = 0; u < s_fam.n_units; u++) {
        fil_txt_t *tx = &s_fil_txt[u];
        pet_ams_unit_name(s_fam.u[u].id, tx->name, sizeof tx->name);
        pet_ams_humidity_text(&s_fam.u[u], tx->hum, sizeof tx->hum);
        for (int s = 0; s < s_fam.u[u].nslot && s < 4; s++) {
            const pet_tray_t *t = &s_fam.slot[s_fam.u[u].first + s];
            fit_text(tx->type[s], sizeof tx->type[s], t->type[0] ? t->type : "?", &font_pet_12, 31);
        }
    }
    for (int i = 0; i < s_fam.n_ext; i++) {
        const pet_tray_t *t = &s_fam.ext[i];
        bool set = (t->flags & PET_TRAY_KNOWN) && !(t->flags & PET_TRAY_EMPTY);
        fit_text(s_ext_txt[i], sizeof s_ext_txt[i], set ? (t->type[0] ? t->type : "?") : "未设置", &font_pet_12, 48);
    }
    fil_layout();
}
void pet_ui_show_filament(bool on, bool show_hint) {
    if (!s_fpage) { if (!on) return; build_filament(); }
    s_fil_on = on; s_fil_hint = show_hint;
    if (!on) pet_ui_filament_set_spinning(false);
    if (on) s_ffirst = 0;
    set_hidden(s_fhint, !show_hint);
    set_hidden(s_fpage, !on);
    if (on) { lv_obj_move_foreground(s_fpage); fil_layout(); }
    s_reapply = true;
    update_header_visibility();
}
bool pet_ui_filament_open(void) { return s_fil_on; }
bool pet_ui_filament_scroll(int dir) {
    int f = pet_ams_scroll(&s_fam, s_ffirst, dir, PET_AMS_VIEW_H);
    if (f < 0) return false;
    s_ffirst = f; fil_layout();
    return true;
}
void pet_ui_filament_reset_scroll(void) { if (s_ffirst) { s_ffirst = 0; if (s_fpage) fil_layout(); } }

// ---- v4.4 底部提示 ----
void pet_ui_set_hints(const char *pages, const char *fil) {   // v4.4:首页没有提示(PM),只有详情/耗材页
    if (s_d_hint && strcmp(lv_label_get_text(s_d_hint), pages)) UI_TXT(s_d_hint, pages);
    if (s_fhint && strcmp(lv_label_get_text(s_fhint), fil)) UI_TXT(s_fhint, fil);
}
void pet_ui_filament_scroll_to_end(void) { while (pet_ui_filament_scroll(1)) {} }
