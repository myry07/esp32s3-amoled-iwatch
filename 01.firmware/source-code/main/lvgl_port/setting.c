#include "lvgl.h"
#include "ui.h"
#include "esp_log.h"

#include "bsp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NS          "display"
#define NVS_KEY_LEVEL   "brightness"   // uint8_t: 0..100
#define NVS_KEY_ENABLED "bl_enabled"   // uint8_t: 0/1

static const char *TAG = "disp_settings";

// 组件句柄
typedef struct {
    lv_obj_t *screen;     // ★ 独立的 page/screen
    lv_obj_t *slider;
    lv_obj_t *label_val;
    lv_obj_t *sw_enable;
    lv_obj_t *btn_reset;
} disp_ui_t;

static disp_ui_t s_ui;

static lv_point_t touch_start_point;  // 记录起始坐标
static bool gesture_detected = false; // 标志位

static void load_page_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    lv_obj_t *new_scr = NULL;

    switch (code)
    {
    case LV_EVENT_PRESSED:
        gesture_detected = false;
        lv_indev_get_point(indev, &touch_start_point);
        break;

    case LV_EVENT_RELEASED:
    {
        lv_point_t touch_end_point;
        lv_indev_get_point(indev, &touch_end_point);

        int dx = touch_end_point.x - touch_start_point.x;
        int dy = touch_end_point.y - touch_start_point.y;

        const int threshold = 15;

        if (abs(dx) < threshold && abs(dy) < threshold)
        {
            // 移动太小，不是滑动
            break;
        }

        gesture_detected = true;

        if (abs(dx) > abs(dy))
        {
            if (dx > threshold)
            {
                ESP_LOGI("gesture", "右滑");
            
            }
            else
            {
                ESP_LOGI("gesture", "左滑");
            }
        }
        else
        {
            if (dy > threshold)
            {
                ESP_LOGI("gesture", "下滑");
                
            }
            else
            {
                ESP_LOGI("gesture", "上滑");
                new_scr = page1_create();
                lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 50, 0, true);
            }
        }

        break;
    }

    case LV_EVENT_CLICKED:
        if (gesture_detected)
        {
            break;
        }
        ESP_LOGI("btn", "点击事件");
        break;

    default:
        break;
    }
}


// 简单节流（避免滑动时频繁写寄存器）
static void set_brightness_throttled(int percent)
{
    static uint32_t last_ms = 0;
    uint32_t now = lv_tick_get();
    const uint32_t interval_ms = 30; // ~33Hz
    if (now - last_ms < interval_ms) {
        return; // 丢弃太密的更新
    }
    last_ms = now;
    bsp_display_brightness_set(percent);
}

static void nvs_save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t nvs_get_u8_default(const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_u8(h, key, &v);
        nvs_close(h);
        if (err == ESP_ERR_NVS_NOT_FOUND) v = def;
    }
    return v;
}

static void update_value_label(int percent)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(s_ui.label_val, buf);
}

static void apply_enable_state(bool enabled)
{
    if (enabled) {
        lv_obj_clear_state(s_ui.slider, LV_STATE_DISABLED);
        bsp_display_backlight_on();
        int v = (int)lv_slider_get_value(s_ui.slider);
        bsp_display_brightness_set(v);
    } else {
        lv_obj_add_state(s_ui.slider, LV_STATE_DISABLED);
        bsp_display_backlight_off();
    }
    nvs_save_u8(NVS_KEY_ENABLED, enabled ? 1 : 0);
}

// 事件回调
static void slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        int v = (int)lv_slider_get_value(s_ui.slider);
        update_value_label(v);
        if (!lv_obj_has_state(s_ui.slider, LV_STATE_DISABLED)) {
            set_brightness_throttled(v);
        }
    } else if (code == LV_EVENT_RELEASED) {
        int v = (int)lv_slider_get_value(s_ui.slider);
        nvs_save_u8(NVS_KEY_LEVEL, (uint8_t)v);
        ESP_LOGI(TAG, "Saved brightness=%d%%", v);
    }
}

static void switch_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool on = lv_obj_has_state(s_ui.sw_enable, LV_STATE_CHECKED);
        apply_enable_state(on);
        ESP_LOGI(TAG, "Backlight %s", on ? "ENABLED" : "DISABLED");
    }
}

static void reset_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        const int def = 100;
        lv_slider_set_value(s_ui.slider, def, LV_ANIM_ON);
        update_value_label(def);
        nvs_save_u8(NVS_KEY_LEVEL, (uint8_t)def);
        if (lv_obj_has_state(s_ui.sw_enable, LV_STATE_CHECKED)) {
            bsp_display_brightness_set(def);
        }
        ESP_LOGI(TAG, "Reset brightness to %d%%", def);
    }
}


// 创建并返回设置页（独立 screen）。已存在则直接返回。
lv_obj_t *settings_page(void)
{
    if (s_ui.screen && lv_obj_is_valid(s_ui.screen)) {
        return s_ui.screen;
    }

    // 确保 NVS 初始化
    static bool nvs_inited = false;
    if (!nvs_inited) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
        nvs_inited = true;
    }

    // 读取默认值
    uint8_t saved_level  = nvs_get_u8_default(NVS_KEY_LEVEL,   100);
    uint8_t saved_enable = nvs_get_u8_default(NVS_KEY_ENABLED, 1);

    // --- Screen 基础：做个微渐变 + 统一内边距 ---
    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(s_ui.screen, 16, 0);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(0x0E0F13), 0);
    lv_obj_set_style_bg_grad_color(s_ui.screen, lv_color_hex(0x171A20), 0);
    lv_obj_set_style_bg_grad_dir(s_ui.screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s_ui.screen, 0, 0);

    // 用列布局把元素竖直排布，间距大一点更“呼吸”
    lv_obj_set_flex_flow(s_ui.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.screen, 16, 0);

    // --- 大数值读数（居中放在标题下）---
    s_ui.label_val = lv_label_create(s_ui.screen);
    update_value_label(saved_level); // 会设置文本
    lv_obj_set_style_text_color(s_ui.label_val, lv_color_hex(0xF6F7F9), 0);
    lv_obj_set_style_text_opa(s_ui.label_val, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(s_ui.label_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_ui.label_val, 1, 0);
    // 稍微加大点视觉体量：用 transform scale（等效放大，不改字体）
    lv_obj_set_style_transform_zoom(s_ui.label_val, 270, 0); // 256为1.0，这里约1.05x

    // --- 滑块行（你现有的 row，但做了“卡片”风格 + 大滑块）---
    lv_obj_t *row = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);         // 垂直堆叠：文字在上，滑块在下
    lv_obj_set_style_pad_all(row, 14, 0);
    lv_obj_set_style_pad_row(row, 10, 0);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1F232B), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(row, 24, 0);
    lv_obj_set_style_shadow_opa(row, 40, 0);
    lv_obj_set_style_shadow_color(row, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_spread(row, 2, 0);
    lv_obj_set_style_shadow_ofs_y(row, 8, 0);

    // 行标题
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Brightness");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xC8CCD5), 0);

    // 大滑块（粗轨道 + 大圆钮）
    s_ui.slider = lv_slider_create(row);
    lv_slider_set_range(s_ui.slider, 0, 100);
    lv_slider_set_value(s_ui.slider, saved_level, LV_ANIM_OFF);
    lv_obj_set_width(s_ui.slider, LV_PCT(92));

    // 主轨道高度更厚一点
    lv_obj_set_style_height(s_ui.slider, 18, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.slider, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.slider, lv_color_hex(0x2A2F38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.slider, LV_OPA_COVER, LV_PART_MAIN);

    // 指示条也加粗，做点高亮
    lv_obj_set_style_height(s_ui.slider, 18, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui.slider, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.slider, lv_color_hex(0x4C89FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s_ui.slider, lv_color_hex(0x61B0FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s_ui.slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    // 大圆钮（更容易拖动）
    lv_obj_set_style_width(s_ui.slider, 28, LV_PART_KNOB);
    lv_obj_set_style_height(s_ui.slider, 28, LV_PART_KNOB);
    lv_obj_set_style_radius(s_ui.slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_ui.slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_ui.slider, 18, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s_ui.slider, 60, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s_ui.slider, lv_color_hex(0x000000), LV_PART_KNOB);
    lv_obj_set_style_shadow_ofs_y(s_ui.slider, 6, LV_PART_KNOB);

    lv_obj_add_event_cb(s_ui.slider, slider_event_cb, LV_EVENT_ALL, NULL);

    // 应用当前状态到硬件
    if (saved_enable) {
        apply_enable_state(true);
        bsp_display_brightness_set(saved_level);
    } else {
        apply_enable_state(false);
    }

    // Page 的通用事件（你原来的 load_page_cb），保持
    lv_obj_add_event_cb(s_ui.screen, load_page_cb, LV_EVENT_ALL, NULL);

    return s_ui.screen;
}