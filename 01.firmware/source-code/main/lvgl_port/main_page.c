#include "lvgl.h"
#include "ui.h"
#include "esp_log.h"

#include "bsp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

static const char *TAG = "main_page";

static lv_obj_t *s_main_page = NULL;

static void video_back_btn_cb(lv_event_t *e);
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
            }
            else
            {
            }
            ESP_LOGI("gesture", "左滑");
            new_scr = page1_create();
            lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 50, 0, true);
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

// Pic 按钮的回调
static void pic_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        ESP_LOGI(TAG, "Picture 被点击");
        lv_obj_t *new_scr = photo_album_create("/spiffs/nr",
                                               BSP_LCD_H_RES,
                                               BSP_LCD_V_RES,
                                               true);
        if (new_scr)
        {
            lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_FADE_IN, 50, 0, true);
        }
        else
        {
            ESP_LOGE(TAG, "创建相册页面失败（目录不存在或无图片）");
            // 可选：弹个提示
            // lv_obj_t *mb = lv_msgbox_create(NULL, "相册",
            //                                 "未找到 /sdcard/nr 或没有 JPG 图片。",
            //                                 NULL, true);
            // lv_obj_center(mb);
        }
    }
}

// video btn
static void video_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;
    ESP_LOGI(TAG, "Video 被点击");
    video_audio_start_on_new_page(); // 这里就会创建新页面并开始播放
}

lv_obj_t *page_main_create(void)
{
    // 1) 页面容器
    s_main_page = lv_obj_create(NULL);
    lv_obj_set_size(s_main_page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_opa(s_main_page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_main_page, lv_color_black(), 0);
    lv_obj_clear_flag(s_main_page, LV_OBJ_FLAG_SCROLLABLE);

    // 2) 背景图（用 lv_img 对象承载）
    const char *bg_path = "/spiffs/cute1.jpg";
    lv_obj_t *bg_img = show_jpg_as_img(s_main_page, bg_path, BSP_LCD_H_RES, BSP_LCD_V_RES);
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
    lv_obj_t *row = lv_obj_create(s_main_page);
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

    // 5) Picture 按钮
    lv_obj_t *s_pic_button = lv_btn_create(row);
    lv_obj_set_size(s_pic_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_pic_button, R, 0);
    lv_obj_set_style_bg_color(s_pic_button, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_pic_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_pic_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_pic_button, 9, 0);
    lv_obj_set_style_clip_corner(s_pic_button, true, 0);

    lv_obj_t *img_pic = show_jpg_as_img(s_pic_button, "/spiffs/btns/photo.jpg", ICON_W, ICON_H);
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

    // 6) Video 按钮
    lv_obj_t *s_video_button = lv_btn_create(row);
    lv_obj_set_size(s_video_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_video_button, R, 0);
    lv_obj_set_style_bg_color(s_video_button, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_video_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_video_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_video_button, 9, 0);
    lv_obj_set_style_clip_corner(s_video_button, true, 0);

    lv_obj_t *img_video = show_jpg_as_img(s_video_button, "/spiffs/btns/video.jpg", ICON_W, ICON_H);
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

    // 7) Music 按钮
    lv_obj_t *s_music_button = lv_btn_create(row);
    lv_obj_set_size(s_music_button, ICON_W, ICON_H);
    lv_obj_set_style_radius(s_music_button, R, 0);
    lv_obj_set_style_bg_color(s_music_button, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(s_music_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_music_button, 12, 0);
    lv_obj_set_style_shadow_ofs_y(s_music_button, 9, 0);
    lv_obj_set_style_clip_corner(s_music_button, true, 0);

    lv_obj_t *img_music = show_jpg_as_img(s_music_button, "/spiffs/btns/music.jpg", ICON_W, ICON_H);
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
    lv_obj_add_event_cb(s_pic_button, pic_btn_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(s_video_button, video_btn_cb, LV_EVENT_CLICKED, NULL);
    // lv_obj_add_event_cb(s_music_button, music_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(s_main_page, load_page_cb, LV_EVENT_ALL, NULL);

    return s_main_page;
}

