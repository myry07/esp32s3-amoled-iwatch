#include "lvgl.h"
#include "ui.h"
#include "esp_log.h"

#include "bsp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"



static const char *TAG = "lock_page";

static lv_obj_t *s_lock_page = NULL;
static lv_obj_t *s_time_label = NULL;

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
                ESP_LOGI("gesture", "右滑");
            else
                ESP_LOGI("gesture", "左滑");
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
                {
                    new_scr = page_main_create();
                    lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, true);
                }
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

void solid_test(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_size(s, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_scr_load(s);

    lv_obj_set_style_bg_color(s, lv_color_hex(0xF800), 0); // 纯红
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(800));

    lv_obj_set_style_bg_color(s, lv_color_hex(0x07E0), 0); // 纯绿
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(800));

    lv_obj_set_style_bg_color(s, lv_color_hex(0x001F), 0); // 纯蓝
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(800));
}

lv_obj_t *page_lock_create(void)
{
    // 1) 不要一开始就上锁
    s_lock_page = lv_obj_create(NULL);
    lv_obj_set_size(s_lock_page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_lock_page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_lock_page, lv_color_black(), 0);
    lv_obj_clear_flag(s_lock_page, LV_OBJ_FLAG_SCROLLABLE);

    // 2) 背景图（show_jpg_on_canvas 内部会自己加锁）
    lv_obj_t *img = show_jpg_as_img(s_lock_page, "/spiffs/4k1.jpg", BSP_LCD_H_RES, BSP_LCD_V_RES);

    // 3) 时间与电量（这些是短操作，加锁-解锁快速包裹一下）
    bsp_display_lock(portMAX_DELAY);
    s_time_label = lv_label_create(s_lock_page);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_40, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(s_time_label, "09:00");

    lv_obj_t *bat_label = lv_label_create(s_lock_page);
    lv_label_set_text(bat_label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(bat_label, LV_ALIGN_TOP_RIGHT, -60, 16);

    lv_obj_add_event_cb(s_lock_page, load_page_cb, LV_EVENT_ALL, NULL);
    bsp_display_unlock();

    // 4) **在未加锁的情况下**做屏幕切换动画
    lv_scr_load_anim(s_lock_page, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, true);

    return s_lock_page;
}