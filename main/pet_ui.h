// main/pet_ui.h — 桌面小宠物界面(LVGL)。所有函数须在 LVGL 任务内或持有 bsp_lvgl_lock() 调用。
#pragma once
#include "pet_logic.h"
void pet_ui_create(void);
void pet_ui_update(pet_state_t st, const printer_status_t *s, link_state_t link);
void pet_ui_react(void);            // 冒爱心

typedef enum { UI_PET = 0, UI_SETUP_QR, UI_BOUND, UI_REAUTH, UI_RESET, UI_EXPIRY } ui_screen_t;
// arg:UI_SETUP_QR = 热点名;UI_BOUND = 打印机名;UI_EXPIRY = 剩余天数("3")
void pet_ui_set_screen(ui_screen_t s, const char *arg);
ui_screen_t pet_ui_screen(void);
// 恢复出厂倒计时:secs_left 5..0,pct 0..100
void pet_ui_set_reset_progress(int secs_left, int pct);

// ---- v4.2 ----
// 顶部机器名 + 在线绿点 + 页码小圆点。name 里有字库没有的字时自动改用 product(再没有就「打印机 N」)。
void pet_ui_set_header(const char *name, const char *product, bool online, int idx, int n);
// 切换机器:机器名横向滑动(dir=+1 新机器从右边进来),下方内容交叉淡入,小圆点同步。
void pet_ui_switch_printer(int dir, const char *name, const char *product, bool online, int idx, int n);
// 详情页(Handy 风格)。show_hint = 底部操作提示是否显示(前 3 次)。
void pet_ui_show_detail(bool on, bool show_hint);
bool pet_ui_detail_open(void);
// v4.3 耗材页:中键 首页 → 详情 → 耗材 → 首页。set_filament 复制一份数据(变了才重排);
// scroll 返回 false = 已到头/到底(调用方改为切换机器)
void pet_ui_set_filament(const pet_ams_t *a);
void pet_ui_show_filament(bool on, bool show_hint);
bool pet_ui_filament_open(void);
bool pet_ui_filament_scroll(int dir);
void pet_ui_filament_reset_scroll(void);
// 打印中(挤出)时在用料盘的中心孔慢转(2 秒一圈);其他状态静止
void pet_ui_filament_set_spinning(bool on);
// 设置页(v4.8 6 行,UI 定稿 v2):演示模式(开/关)、休眠(关/2/5 分钟)、熄屏画面(全黑/时钟/线线)、重新配网、返回、恢复出厂设置
typedef enum { SET_DEMO = 0, SET_SLEEP, SET_FACE, SET_REPAIR, SET_BACK, SET_RESET, SET_COUNT } set_item_t;   // v4.5.3:恢复出厂固定在最后(返回之后),红字
// 行布局集中在这里(改版只改这几行):rbox(s_set, SET_ROW_X, y, SET_ROW_W, SET_ROW_H, 10, col),y = SET_ROW_Y0 + i*SET_ROW_STEP;
// 文字 font_pet_18 在行内 (SET_TXT_DX, SET_TXT_DY)。标题 y=24、底部提示 y=SET_HINT_Y 不变;最后一行底边 54+5*38+34-1 = 277。
#define SET_ROW_X     20
#define SET_ROW_W     200
#define SET_ROW_Y0    54
#define SET_ROW_STEP  38
#define SET_ROW_H     34
#define SET_TXT_DX    14
#define SET_TXT_DY    6
#define SET_HINT_Y    284
static inline int pet_set_row_y(int i) { return SET_ROW_Y0 + i * SET_ROW_STEP; }
void pet_ui_show_settings(bool on, int sel, bool demo_on, uint8_t sleep_min, uint8_t sleep_face);
// 设置页文案见 sleep_logic.h pet_set_sleep_label / pet_set_face_label
bool pet_ui_settings_open(void);
// 演示模式右上角「n/6」;step<0 隐藏
void pet_ui_set_demo_step(int step);
// v4.4:底部提示文字(pages = 详情页,fil = 耗材页);首页没有提示
void pet_ui_set_hints(const char *pages, const char *fil);
void pet_ui_filament_scroll_to_end(void);                   // 从首页按上键进耗材页:停在最后一屏
// v4.4.1:没数据时整页「连接中/未连上」(详情/耗材页也一样);有数据 = 正常页面 + 实心绿点
void pet_ui_set_conn(pet_conn_state_t c);
pet_conn_state_t pet_ui_conn_shown(void);

// ---- v4.8 熄屏画面 B(时钟)/ C(线线)----
// 进入:建一个独立的全屏黑底页面并加载,隐藏状态条(lv_layer_top),停角色动画/闪烁定时器;
// 退出:回到原页面、删掉熄屏页面、恢复状态条和定时器。A(全黑)不用这些(只暂停刷屏)。
typedef struct {
    char time[8];         // "HH:MM" 或 "--:--"
    int soc;              // 0..100;<0 显示 "--%"
    bool printing;        // B:有打印中的机器
    char name[48];        // B:机器名(原名,UI 里按字库回退)
    char product[24];
    int idx;              // B:机器序号(回退「打印机 N」用)
    int pct;              // B:进度 0..100
    int dx, dy;           // 防残影位移
} pet_sleep_info_t;
void pet_ui_sleep_enter(int face, const pet_sleep_info_t *info);
void pet_ui_sleep_update(const pet_sleep_info_t *info);
void pet_ui_sleep_exit(void);
// A/全部:暂停/恢复角色动画与闪烁定时器(C 和 A 都停;不释放对象)
void pet_ui_anim_pause(bool pause);
// A:暂停/恢复 LVGL 刷屏(关失效登记 + 暂停刷新定时器,不释放对象);恢复时整屏失效
void pet_ui_render_pause(bool pause);
// 立刻整屏刷新一次(lv_refr_now);调用后像素可能仍在 SPI 队列里,开背光前用 bsp_display_wait_idle/bsp_display_on 等完
void pet_ui_refresh_now(void);
