// main/pet_logic.h — 宠物状态机与拓竹状态映射。纯 C,不依赖 ESP-IDF/LVGL,有主机测试。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PET_IDLE = 0,   // 打盹
    PET_PRINT,      // 干活
    PET_DONE,       // 撒花,直到按键确认
    PET_PAUSE,      // 喝茶
    PET_EMPTY,      // 缺料
    PET_FAIL,       // 失败
    PET_STATE_COUNT
} pet_state_t;

typedef enum { LINK_DEMO = 0, LINK_UNCONFIGURED, LINK_CONNECTING, LINK_ONLINE, LINK_OFFLINE, LINK_AUTH_FAIL } link_state_t;

// token 是否该刷新:离过期不到 margin 秒(exp/now 为 Unix 秒,exp=0 表示未知)。
bool pet_token_needs_refresh(int64_t exp, int64_t now, int64_t margin);
// 剩余天数(向上取整;已过期返回 0;exp 未知或未对时返回 -1)。
int pet_token_days_left(int64_t exp, int64_t now);
// 过期提醒:剩余 1..3 天,且今天(本地日序号)还没提醒过。
bool pet_expiry_should_remind(int days_left, int32_t today, int32_t last_reminded_day);
// 测试版:剩余分钟(向上取整;已过期 0;未知 -1)
int pet_token_minutes_left(int64_t exp, int64_t now);
// 打印进度 -> 模型顶面 y(床面 bed_y,100% 时到 top_y)。
int pet_model_top_y(int percent, int bed_y, int top_y);

typedef struct {
    bool valid;
    char gcode_state[16];     // IDLE/PREPARE/RUNNING/PAUSE/FINISH/FAILED/SLICING...
    int percent;              // mc_percent, -1 未知
    int remaining_min;        // mc_remaining_time, -1 未知
    int layer, total_layer;   // -1 未知
    float nozzle, nozzle_target, bed, bed_target;
    uint32_t print_error;     // 0 = 无
    bool hms_runout;          // hms 数组里出现缺料码
    uint32_t fil_rgb;         // 当前在用耗材颜色 0xRRGGBB;0 = 未知(用默认)
    char subtask[48];         // 文件名(只显示 ASCII,中文名不保证字形)
    char job_id[24];          // subtask_id(没有时用 task_id);只在 push_all 全量报文里出现
    int elapsed_min;          // 本次打印用时(分钟),由 pet_job 记录得出;-1 = 不知道开始时间
    // ---- v4.2 详情页 ----
    int n_nozzles;            // 1 = 单喷嘴(P1/X1/A1);2 = H2 双喷嘴。0 = 还没收到温度
    int active_nozzle;        // UI 序号:0 = 左, 1 = 右(单喷嘴恒为 0);nozzle/nozzle_target 就是它
    float noz[2], noz_t[2];   // UI 序号 0 = 左喷嘴, 1 = 右喷嘴(单喷嘴只用 [0])
    bool has_chamber;
    float chamber, chamber_target;
    bool chamber_has_target;   // v4.4.1:H2 ctc 打包温度带目标;X1 chamber_temper 没有目标(显示「/--」)
    char fil_type[16];        // tray_type,如 "PLA";空 = 未知
    char fil_slot[8];         // "HT-A" / "A1" / "外挂";空 = 未知
    int fil_humidity;         // 所在 AMS 的 humidity_raw(%);-1 = 未知
} printer_status_t;

#define PET_MAX_PRINTERS 4
// 温度文案:target>0 且还在升温(cur < target-2)时 "36→220°C",否则只有当前 "36°C"(不会出现 39/0°C)
void pet_format_temp(float cur, float target, char *buf, size_t len);
// 详情页进度条右侧的状态字:成功/打印中/暂停/断料/失败/准备中/空闲
const char *pet_status_word(const printer_status_t *s, pet_state_t st);
// 耗材颜色 -> 中文颜色名(最近色,HSV 粗分):白色/黑色/灰色/红色/橙色/黄色/绿色/青色/蓝色/紫色/粉色/棕色
const char *pet_color_name(uint32_t rgb);
// AMS 槽位标签:ams_id 0..3 -> "A1".."D4";128..135(AMS HT)-> "HT-A".."HT-H";254/255 -> "外挂"
void pet_tray_label(int ams_id, int slot, char *buf, size_t len);

// ---- 多机自动跳转(断料/失败/完成):事件发生后停 3 秒再跳;两次跳转至少隔 3 秒,不来回抖 ----
#define PET_JUMP_HOLD_MS 3000
typedef struct {
    int n, cur;
    pet_state_t last[PET_MAX_PRINTERS];
    uint8_t queued;            // 有待跳转事件的机器位图
    int pending;               // 正在倒计时的目标,-1 = 无
    uint32_t due;              // pending 到期时刻(ms)
    uint32_t next_ok;          // 下一次允许跳转的最早时刻
    uint32_t ev_at[PET_MAX_PRINTERS];   // 各机器事件发生时刻
} pet_switch_t;
void pet_switch_init(pet_switch_t *w, int n);
// 每个循环喂一次各机器当前(锁存后)的状态;返回要跳去的机器序号,不跳返回 -1(调用方随后会调 pet_switch_set_cur)。
int pet_switch_tick(pet_switch_t *w, const pet_state_t *st, uint32_t now_ms);
// 用户手动切换(或自动跳转完成):更新当前机器,取消到它的事件,其他排队事件重新等 3 秒
void pet_switch_set_cur(pet_switch_t *w, int idx, uint32_t now_ms, bool manual);

// ---- 打印用时:开始时间按 job_id 记录(调用方持久化到 NVS,重启后同一 job 仍能算出用时) ----
typedef struct {
    char job[24];
    int64_t start;            // 0 = 未知(例如开机时已经打到一半、又没有记录)
    int64_t end;              // FINISH/FAILED 时刻;0 = 还没结束
} pet_job_t;
#define PET_TIME_VALID 1700000000LL   // 早于此的 now 视为时钟未同步
void pet_job_init(pet_job_t *j);
// 每次拿到状态时调用。返回 true 表示记录变了(调用方写 NVS)。
// 只有"亲眼看到开头"才记开始时间:新 job 首次出现时进度 <=1% 或处于 PREPARE;否则开始时间未知。
bool pet_job_update(pet_job_t *j, const printer_status_t *s, int64_t now);
// 用时(分钟,四舍五入),未知返回 -1。job 不匹配当前状态也返回 -1。
int pet_job_elapsed_min(const pet_job_t *j, const printer_status_t *s);
// "3h 12m" / "0h 05m";min<0 -> "--"
void pet_format_duration(int min, char *buf, size_t len);

void printer_status_reset(printer_status_t *s);

// print_error(32 位,如 0x07008011)是否为缺料。
bool pet_is_runout_print_error(uint32_t code);
// hms 数组单项(attr, code)是否为缺料。
bool pet_is_runout_hms(uint32_t attr, uint32_t code);

// 原始映射(不含"完成需确认"的锁存)。
pet_state_t pet_state_from_status(const printer_status_t *s);

// 锁存器:FINISH 首次出现时显示 DONE,直到 pet_machine_ack();之后回到 IDLE。
typedef struct {
    pet_state_t shown;
    bool done_acked;
    bool seen_active;  // 本次上电后看到过打印中,避免开机时旧的 FINISH 也撒花
} pet_machine_t;
void pet_machine_init(pet_machine_t *m);
pet_state_t pet_machine_update(pet_machine_t *m, const printer_status_t *s);
// 按键确认;返回 true 表示确认了"完成"状态。
bool pet_machine_ack(pet_machine_t *m);

// 演示数据:第 step 步(0..5 对应六种状态)。
void pet_fake_status(int step, uint32_t tick, printer_status_t *out);
// 演示第二台:「（示例）第二台 P1S」单喷嘴、一直在打印
void pet_fake_status_p2(uint32_t tick, printer_status_t *out);

// "还剩 1小时12分" / "还剩 8分钟";min<0 返回 "--"。
#define PET_DEFAULT_FIL_RGB 0xE64646u
// 换色兜底:拿不到耗材颜色(外挂料盘/没有 AMS 数据)时 #FF00FF 区域用浅灰。
// 不用纯白:换色区外面那圈 1px 描边就是 #FFFFFF,白色会和描边糊成一片。
#define PET_RECOLOR_FALLBACK_RGB 0xDCDCDCu
#define PET_EMPTY_NOFIL_RGB 0x969696u   // v4.6 empty 状态没料兜底灰
#define PET_RECOLOR_MIN_LUMA 64   // 25% of 255 (Rec.709 luma)
// Rec.709 luma 0..255 of 0xRRGGBB.
int pet_luma(uint32_t rgb);
// Colour painted into #FF00FF regions: fil_rgb==0 -> fallback; luma < 25% -> blended toward white
// just enough to reach 25% (hue kept, black -> dark grey); otherwise unchanged.
uint32_t pet_recolor_rgb(uint32_t fil_rgb);
// #C000C0 阴影键:在 pet_recolor_rgb 结果上 *0.7;近黑耗材(fil==0 或 luma<MIN)则提亮(~/0.7)而非压暗。
uint32_t pet_recolor_shade_rgb(uint32_t fil_rgb);
#define PET_SHADE_KEY_RGB 0xC000C0u
// 解析拓竹 tray_color "RRGGBBAA";失败返回 0。
uint32_t pet_parse_tray_color(const char *s);

void pet_format_remaining(int min, char *buf, size_t len);
// print_error -> 短中文原因(只用字库里已有的字)。
const char *pet_error_text(uint32_t code);

// ---- v4.3 耗材页:每台打印机的 AMS 状态(紧凑存储,约 400 B/台) ----
#define PET_AMS_MAX_UNITS 12      // 4 台 AMS / AMS 2 Pro(id 0..3)+ 8 台 AMS HT(id 128..135)
#define PET_AMS_MAX_SLOTS 24      // 4 x 4 + 8 x 1
#define PET_AMS_MAX_EXT   2       // 外挂料盘:H2 左 / 右,其他机型 1 个
enum {
    PET_TRAY_KNOWN  = 1,          // 收到过这个槽的数据
    PET_TRAY_EMPTY  = 2,          // 空槽(tray_exist_bits 清零 / 只有 id / tray_type 为空)
    PET_TRAY_ACTIVE = 4,          // 正在用(H2:任一喷嘴的 snow;其他:tray_now)
    PET_TRAY_SIDE_L = 8,          // 外挂料盘:左(vir_slot id 254)
    PET_TRAY_SIDE_R = 16,         // 外挂料盘:右(vir_slot id 255)
    PET_TRAY_NOZ_L  = 32,         // H2:左喷嘴在用这个槽(snow of extruder id 1)
    PET_TRAY_NOZ_R  = 64,         // H2:右喷嘴在用这个槽(snow of extruder id 0)
};
typedef struct {
    uint8_t r, g, b;              // tray_color RRGGBBAA 的 RGB
    uint8_t flags;                // PET_TRAY_*
    int8_t remain;                // 剩余 %,-1 = 未知
    char type[8];                 // tray_type(截断到 7 字节),如 "PLA"
} pet_tray_t;
typedef struct {
    uint8_t id;                   // 0..3 = AMS-A..D;128..135 = HT-A..H
    int8_t hum_pct;               // humidity_raw(1..100),-1 = 没有
    int8_t hum_lvl;               // humidity 等级(1..5),-1 = 没有
    uint8_t first, nslot;         // 槽在 pet_ams_t.slot[] 里的起点和个数(AMS 4 个,HT 1 个)
} pet_ams_unit_t;
typedef struct {
    bool valid;                   // 收到过 print.ams.ams[]
    uint8_t n_units, n_slots, n_ext;
    pet_ams_unit_t u[PET_AMS_MAX_UNITS];
    pet_tray_t slot[PET_AMS_MAX_SLOTS];
    pet_tray_t ext[PET_AMS_MAX_EXT];
} pet_ams_t;
void pet_ams_reset(pet_ams_t *a);
bool pet_ams_is_ht(int id);
// "AMS-A".."AMS-D" / "HT-A".."HT-H";其他 id 显示 "AMS-<id>"
void pet_ams_unit_name(int id, char *buf, size_t len);
// 湿度文字:"39%";没有百分比时用等级 "4级";都没有返回 ""
void pet_ams_humidity_text(const pet_ams_unit_t *u, char *buf, size_t len);
// 深色耗材(亮度 < 48)画灰色外圈,黑色料盘在深色卡片上才看得见
bool pet_tray_dark(const pet_tray_t *t);
// 余量红点:0 <= remain <= 15;-1 未知不显示;空槽不显示
bool pet_tray_low(const pet_tray_t *t);
// 在用标记:清掉所有 ACTIVE/NOZ_* 后按喷嘴料槽重新标(snow 编码 ams<<8|slot;254/255 + slot 0 = 外挂左/右)。
// n_snow == 2:H2,snow[0] = extruder id 0(右喷嘴),snow[1] = id 1(左喷嘴),额外标 NOZ_R / NOZ_L;n_snow == 1:单喷嘴
void pet_ams_mark_active(pet_ams_t *a, const int *snow, int n_snow);
// 在用标签文字:"L" / "R" / "LR"(两个喷嘴同一槽);单喷嘴或不在用返回 ""(单喷嘴时 UI 只画绿框,不画标签)
const char *pet_tray_tag(const pet_tray_t *t);
// 耗材页行:每台 AMS 一行(PET_AMS_ROW_H),有外挂料盘时最后一行(PET_AMS_EXT_H)
#define PET_AMS_ROW_H  56
#define PET_AMS_EXT_H  28
#define PET_AMS_GAP    6
#define PET_AMS_VIEW_H 214        // y 70..284
int pet_ams_row_count(const pet_ams_t *a);
int pet_ams_row_h(const pet_ams_t *a, int row);
// 从 first 开始能完整放下几行(至少 1)
int pet_ams_rows_fit(const pet_ams_t *a, int first, int view_h);
// 上下键:dir +1 = 下,-1 = 上。能翻页返回新的 first;已经到头/到底(或一屏放得下)返回 -1,调用方改为切换机器
int pet_ams_scroll(const pet_ams_t *a, int first, int dir, int view_h);
// 演示数据:printer 0 = 示例 H2C(AMS-A/B + HT-A,9 个槽);1 = 示例 P1S(1 台 AMS)
void pet_fake_ams(int printer, pet_ams_t *out);

// ---- v4.4 按键:中键单击/双击/长按(纯逻辑,主机可测)----
// 双击判定只在 >= 2 台打印机且不在演示模式时开启;窗口 = 上次松开到下次按下 <= PET_DBL_MS。
// 关闭时松开立即上报单击(不等 250ms)。长按由按键驱动上报,一旦长按,本次松开不算单击。
#define PET_DBL_MS 250
typedef enum { PET_OK_NONE = 0, PET_OK_SINGLE, PET_OK_DOUBLE } pet_ok_ev_t;
typedef struct { bool down, long_fired, pending, second; uint32_t t_up; } pet_okkey_t;
void pet_okkey_init(pet_okkey_t *k);
pet_ok_ev_t pet_okkey_press(pet_okkey_t *k, uint32_t now);
void pet_okkey_long(pet_okkey_t *k);
pet_ok_ev_t pet_okkey_release(pet_okkey_t *k, uint32_t now, bool dbl_enabled);
pet_ok_ev_t pet_okkey_tick(pet_okkey_t *k, uint32_t now);   // 窗口到期:补发挂起的单击
bool pet_dbl_enabled(bool demo, int n_printers);
// 页面:0 首页 → 1 详情 → 2 耗材 → 0 循环;dir +1 = 下键(下一页),-1 = 上键
#define PET_PAGES 3
int pet_page_step(int page, int dir);
// 底部提示文字。page 0/1 = 「上下键切页 · 双击中键换机器」;2 = 「上下键翻页/切页 · 双击中键换机器」;
// demo = 「中键切状态 n/6 · 上下键切页」(step 0..5);只有 1 台打印机时去掉「 · 双击中键换机器」
const char *pet_hint_text(int page, bool demo, int n_printers, int step, char *buf, size_t len);
// v4.4 PM:底部提示只在详情/耗材页且常亮(首页没有);演示角标「n/6」只在首页
bool pet_hint_on_page(int page);
bool pet_demo_badge_on_page(int page);
// v4.4 设计规则:打印机小圆点只在 >= 2 台时画;1 台(演示或正常)一个都不画,机器名位置不变
bool pet_dots_visible(int n_printers);
// v4.4.1 详情页温度卡(设计 detail_v441):当前值(15px 白色粗体数字,不带单位)+ 紧跟 12px 灰色「/目标°C」。
// 空闲目标为 0 也显示「30/0°C」,绝不因为目标是 0 就把当前值显示成 0;没有目标(X1 仓温)显示「/--」;没数据 cur="--"、tgt=""。
void pet_temp_pair(bool valid, float cur, float target, bool has_target, char *cbuf, size_t clen, char *tbuf, size_t tlen);
// 双喷嘴卡片:报文里有 2 个喷嘴,或者型号是 H2 系列(没数据时也先显示 L/R 两张卡)
bool pet_is_dual(int n_nozzles, const char *product);
// ---- v4.5.3 恢复出厂:防误触状态机 ----
// 只能从设置页「恢复出厂设置」单击进入确认页(不计时);必须先松开中键、再重新按下才从 0 开始计时;
// 按满 5 秒才清除;不到 5 秒松手 = 取消回设置页。进确认页时如果中键还按着(长按进设置页带过来的),那一次按住永远不计。
#define PET_RESET_HOLD_MS 5000
typedef enum { PET_RST_OFF = 0, PET_RST_WAIT_RELEASE, PET_RST_READY, PET_RST_HOLDING, PET_RST_DONE } pet_rst_state_t;
typedef enum { PET_RST_EV_NONE = 0, PET_RST_EV_CANCEL, PET_RST_EV_RESET } pet_rst_ev_t;
typedef struct { pet_rst_state_t st; uint32_t t0; } pet_reset_t;
void pet_reset_init(pet_reset_t *r);
void pet_reset_open(pet_reset_t *r, bool ok_down);              // 打开确认页;ok_down = 中键此刻是否还按着
void pet_reset_close(pet_reset_t *r);
void pet_reset_press(pet_reset_t *r, uint32_t now);
pet_rst_ev_t pet_reset_release(pet_reset_t *r, uint32_t now);   // 计时中松手 = CANCEL
pet_rst_ev_t pet_reset_tick(pet_reset_t *r, uint32_t now);      // 按满 5 秒 = RESET(只报一次)
uint32_t pet_reset_elapsed(const pet_reset_t *r, uint32_t now); // 计时中已按住的毫秒(否则 0)
// 设置页上下选择:到头/到底停住,不回绕(从第一项往上不会跳到最后的项)
int pet_set_step(int sel, int dir, int count);
// ---- v4.5.2 配网热点名:屏幕文字、二维码 WIFI: 串、实际 SSID 都只从这里来 ----
#define PET_AP_PREFIX "Passport-"
void pet_ap_name(const uint8_t mac[6], char *out, size_t len);          // "Passport-%02X%02X"(SoftAP MAC 后两字节)
void pet_ap_qr(const char *ap, char *out, size_t len);                  // "WIFI:T:nopass;S:<ap>;;"
void pet_ap_step(const char *ap, char *out, size_t len);                // "连上热点 <ap>"
// ---- v4.4.1 没数据时的整页「连接中 / 未连上」----
// 进入"没数据"后前 30 秒 = 连接中;超过 30 秒 = 未连上;收到第一条带温度的完整报文立刻 = 有数据(不做过渡)。
// 数据丢了(在线窗口过期/断线)重新从「连接中」计 30 秒。演示模式永远算有数据。
#define PET_CONN_FAIL_MS 30000
typedef enum { PET_CONN_DATA = 0, PET_CONN_CONNECTING, PET_CONN_FAILED, PET_CONN_NODATA } pet_conn_state_t;
typedef struct { bool started, had_data; uint32_t t0; } pet_conn_t;
void pet_conn_init(pet_conn_t *c);
pet_conn_state_t pet_conn_update(pet_conn_t *c, bool has_data, uint32_t now_ms);
// v4.4.3「读不到打印机数据」(设计 conn.py:5_conn_nodata):MQTT 已连上,但报文解析失败,或连上后 30 秒还没有温度。
// 「未连上拓竹云」只留给 Wi-Fi / 打印机列表 / MQTT 连接这几步失败(mqtt_up=false 且超过 30 秒)。第一条完整数据立刻回正常。
pet_conn_state_t pet_conn_update_ex(pet_conn_t *c, bool has_data, bool mqtt_up, bool parse_fail, uint32_t now_ms);
// 绿点:只有 PET_CONN_DATA 才是实心绿点;连接中 = 灰色空心圈;未连上 = 红色空心圈
