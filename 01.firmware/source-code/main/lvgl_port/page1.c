#include "lvgl.h"
#include "ui.h"
#include "esp_log.h"

#include "bsp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "game1.h"

static const char *TAG = "page1";

static lv_obj_t *s_page1 = NULL;

// static void video_back_btn_cb(lv_event_t *e);
// static void bg_img_delete_cb(lv_event_t *e);

typedef struct
{
    char path[256];
    bool is_dir;
    bool loop;
    lv_obj_t *back_btn;
    lv_obj_t *title;
} video_page_ctx_t;

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
                new_scr = page_main_create();
                lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 50, 0, true);
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
                new_scr = page_lock_create();
                lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, true);
            }
            else
            {
                ESP_LOGI("gesture", "上滑");
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

static void back_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    lv_obj_t *prev_scr = (lv_obj_t *)lv_event_get_user_data(e);
    if (!prev_scr)
        return;

    // 切回上一页，并删除当前（游戏）页
    lv_scr_load_anim(prev_scr, LV_SCR_LOAD_ANIM_FADE_OUT, 200, 0, /*auto_del=*/true);
}

// Game1 按钮的回调
static void game1_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    lv_obj_t *prev = lv_screen_active();   // 记录当前页

    lv_obj_t *new_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(new_scr, lv_color_black(), 0);

    lv_obj_t *game = lv_100ask_2048_create(new_scr);
    lv_obj_set_size(game, LV_PCT(100), LV_PCT(100));
    lv_obj_center(game);

    // 返回按钮
    lv_obj_t *back = lv_btn_create(new_scr);
    lv_obj_set_size(back, 78, 42);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 80, -30);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, prev);
    lv_obj_t *lbl = lv_label_create(back);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl);

    // 关键：不要删除 prev（auto_del=false）
    lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}


static void setting_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *new = settings_page();  
    lv_scr_load_anim(new, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

lv_obj_t *page1_create(void)
{
    // 1) 页面容器
    s_page1 = lv_obj_create(NULL);
    lv_obj_set_size(s_page1, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_page1, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_page1, lv_color_black(), 0);
    lv_obj_clear_flag(s_page1, LV_OBJ_FLAG_SCROLLABLE);

    // 2) 背景图（用 lv_img 对象承载）
    const char *bg_path = "/spiffs/cute2.jpg";
    lv_obj_t *bg_img = show_jpg_as_img(s_page1, bg_path, BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (bg_img)
    {
        // 背景不吃事件，移到最底层
        lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_background(bg_img);
        lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);

        // 把动态图片指针随对象保存，供销毁时释放
#if LVGL_VERSION_MAJOR >= 9
        lv_obj_set_user_data(bg_img, (void *)lv_image_get_src(bg_img));
#else
        lv_obj_set_user_data(bg_img, (void *)lv_img_get_src(bg_img));
#endif

        // lv_obj_add_event_cb(bg_img, bg_img_delete_cb, LV_EVENT_DELETE, NULL);
    }

    // 3) 顶部一行容器（放 3 个按钮）
    lv_obj_t *row = lv_obj_create(s_page1);
    lv_obj_set_size(row, LV_PCT(100), 140);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // 4) 公共样式
    const int ICON_W = 90, ICON_H = 90, R = 20;

    // 5) Setting 按钮
    lv_obj_t *s_setting_button = lv_btn_create(row);
    lv_obj_set_size(s_setting_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_setting_button, R, 0);
    lv_obj_set_style_bg_color(s_setting_button, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_setting_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_setting_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_setting_button, 9, 0);
    lv_obj_set_style_clip_corner(s_setting_button, true, 0);

    lv_obj_t *img_pic = show_jpg_as_img(s_setting_button, "/spiffs/btns/setting.jpg", ICON_W, ICON_H);
    if (img_pic)
    {
        lv_obj_center(img_pic);
        lv_obj_set_style_radius(img_pic, R, 0);
        lv_obj_set_style_clip_corner(img_pic, true, 0);
    }
    else
    {
        ESP_LOGW(TAG, "photo.JPG load failed, fallback color only");
    }

    // 6) 2048 按钮
    lv_obj_t *s_game1_button = lv_btn_create(row);
    lv_obj_set_size(s_game1_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_game1_button, R, 0);
    lv_obj_set_style_bg_color(s_game1_button, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_game1_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_game1_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_game1_button, 9, 0);
    lv_obj_set_style_clip_corner(s_game1_button, true, 0);

    lv_obj_t *img_video = show_jpg_as_img(s_game1_button, "/spiffs/btns/game1.jpg", ICON_W, ICON_H);
    if (img_video)
    {
        lv_obj_center(img_video);
        lv_obj_set_style_radius(img_video, R, 0);
        lv_obj_set_style_clip_corner(img_video, true, 0);
    }
    else
    {
        ESP_LOGW(TAG, "photo.JPG load failed, fallback color only");
    }

    // 7) Fly bird 按钮
    lv_obj_t *s_game2_button = lv_btn_create(row);
    lv_obj_set_size(s_game2_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_game2_button, R, 0);
    lv_obj_set_style_bg_color(s_game2_button, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(s_game2_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_game2_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_game2_button, 9, 0);
    lv_obj_set_style_clip_corner(s_game2_button, true, 0);

    lv_obj_t *img_music = show_jpg_as_img(s_game2_button, "/spiffs/btns/music.jpg", ICON_W, ICON_H);
    if (img_music)
    {
        lv_obj_center(img_music);
        lv_obj_set_style_radius(img_music, R, 0);
        lv_obj_set_style_clip_corner(img_music, true, 0);
    }
    else
    {
        ESP_LOGW(TAG, "photo.JPG load failed, fallback color only");
    }

    // 8) 事件绑定
    lv_obj_add_event_cb(s_setting_button, setting_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_game1_button, game1_btn_cb, LV_EVENT_CLICKED, NULL);
    // lv_obj_add_event_cb(s_music_button, music_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(s_page1, load_page_cb, LV_EVENT_ALL, NULL);

    return s_page1;
}


