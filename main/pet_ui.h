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
// 设置页:items 依次为 演示模式(开/关)、重新配网、恢复出厂设置、返回
typedef enum { SET_DEMO = 0, SET_REPAIR, SET_BACK, SET_RESET, SET_COUNT } set_item_t;   // v4.5.3:恢复出厂固定在最后(返回之后),红字
void pet_ui_show_settings(bool on, int sel, bool demo_on);
bool pet_ui_settings_open(void);
// 演示模式右上角「n/6」;step<0 隐藏
void pet_ui_set_demo_step(int step);
// v4.4:底部提示文字(pages = 详情页,fil = 耗材页);首页没有提示
void pet_ui_set_hints(const char *pages, const char *fil);
void pet_ui_filament_scroll_to_end(void);                   // 从首页按上键进耗材页:停在最后一屏
// v4.4.1:没数据时整页「连接中/未连上」(详情/耗材页也一样);有数据 = 正常页面 + 实心绿点
void pet_ui_set_conn(pet_conn_state_t c);
pet_conn_state_t pet_ui_conn_shown(void);
