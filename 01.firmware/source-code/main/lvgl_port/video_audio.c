#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "lv_demos.h"
#include "esp_jpeg_dec.h"
#include "avi_player.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>

#include "ui.h"

static const char *TAG = "video_audio";

#define DISP_WIDTH 320
#define DISP_HEIGHT 200

static lv_obj_t *canvas = NULL;
static lv_color_t *canvas_buf[2] = {NULL};
static int current_buf_idx = 0;
static bool loop_playback = true;
static bool is_playing = false;

static jpeg_dec_handle_t jpeg_handle = NULL;

static char **avi_file_list = NULL;
static int avi_file_count = 0;

static int frame_w = 0, frame_h = 0;

#define LVGL_PORT_INIT_CONFIG()   \
    {                             \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,  \
        .task_affinity = -1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

// ---- 全局/静态 ----
static lv_obj_t *s_video_page = NULL;
static volatile bool s_video_stop_req = false;
static TaskHandle_t s_video_start_task = NULL;
static volatile bool s_video_task_exited = false;

static void video_back_btn_cb(lv_event_t *e);

// 在新页面上创建 UI（顶部返回+内容容器+状态标签），返回状态标签
static lv_obj_t *create_video_page_and_get_status_label(void)
{
    s_video_stop_req = false;

    // 新 screen
    s_video_page = lv_obj_create(NULL);
    lv_obj_set_size(s_video_page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_video_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_video_page, LV_OPA_COVER, 0);

    // 返回

    lv_obj_t *btn_back = lv_btn_create(s_video_page);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 100, 0);
    lv_obj_add_event_cb(btn_back, video_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_back);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl);

    // 内容容器
    lv_obj_t *content = lv_obj_create(s_video_page);
    lv_obj_set_size(content, LV_PCT(100), BSP_LCD_V_RES - 60);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    // 关键：内容容器不吃点击，避免盖住按钮
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 再确保返回按钮在最上层（以防其他控件后续创建）
    lv_obj_move_foreground(btn_back);

    // 初始状态标签（后面挂载/扫描时复用）
    lv_obj_t *status = lv_label_create(content);
    lv_obj_set_width(status, BSP_LCD_H_RES - 20);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_20, 0);
    lv_label_set_text(status, "准备开始播放…");
    lv_obj_center(status);

    // 切换到新页面
    lv_scr_load_anim(s_video_page, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
    return status;
}

static bool has_ext_ci(const char *name, const char *ext)
{
    const char *p = strrchr(name, '.');
    if (!p)
        return false;
    while (*p && *ext)
    {
        char a = (char)tolower((unsigned char)*p++);
        char b = (char)tolower((unsigned char)*ext++);
        if (a != b)
            return false;
    }
    return *p == '\0' && *ext == '\0';
}

static esp_err_t get_avi_file_list(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return ESP_FAIL;
    }

    // 第一次遍历只数 .avi（不看 d_type）
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue; // 忽略隐藏项
        if (has_ext_ci(entry->d_name, ".avi"))
            count++;
    }

    if (count == 0)
    {
        closedir(dir);
        ESP_LOGW(TAG, "No AVI files found in directory %s", dir_path);
        return ESP_FAIL;
    }

    char **list = (char **)malloc(sizeof(char *) * count);
    if (!list)
    {
        closedir(dir);
        ESP_LOGE(TAG, "Failed to allocate memory for file list");
        return ESP_ERR_NO_MEM;
    }

    // 第二次遍历填充完整路径
    rewinddir(dir);
    int idx = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        if (!has_ext_ci(entry->d_name, ".avi"))
            continue;

        size_t dlen = strlen(dir_path);
        size_t flen = strlen(entry->d_name);
        bool has_sep = (dlen > 0 && (dir_path[dlen - 1] == '/' || dir_path[dlen - 1] == '\\'));
        size_t need = dlen + (has_sep ? 0 : 1) + flen + 1;

        char *full = (char *)malloc(need);
        if (!full)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for file path");
            for (int k = 0; k < idx; k++)
                free(list[k]);
            free(list);
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        if (has_sep)
            snprintf(full, need, "%s%s", dir_path, entry->d_name);
        else
            snprintf(full, need, "%s/%s", dir_path, entry->d_name);

        // 可选：用 stat 过滤掉目录（保险）
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
        {
            free(full);
            continue;
        }

        list[idx++] = full;
        if (idx >= count)
            break;
    }
    closedir(dir);

    // 若中间因为 stat 过滤掉了一些，修正 avi_file_count
    avi_file_list = list;
    avi_file_count = idx;

    ESP_LOGI(TAG, "Found %d AVI files in %s", avi_file_count, dir_path);
    for (int i = 0; i < avi_file_count; i++)
    {
        ESP_LOGI(TAG, "AVI[%d/%d]: %s", i + 1, avi_file_count, avi_file_list[i]);
    }

    return (avi_file_count > 0) ? ESP_OK : ESP_FAIL;
}

// static esp_err_t get_avi_file_list(const char *dir_path)
// {
//     DIR *dir = opendir(dir_path);
//     if (!dir)
//     {
//         ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
//         return ESP_FAIL;
//     }

//     struct dirent *entry;
//     int count = 0;

//     while ((entry = readdir(dir)) != NULL)
//     {
//         if (entry->d_type == DT_REG)
//         {
//             char *ext = strrchr(entry->d_name, '.');
//             if (ext && (strcasecmp(ext, ".avi") == 0))
//             {
//                 count++;
//             }
//         }
//     }

//     if (count == 0)
//     {
//         closedir(dir);
//         ESP_LOGW(TAG, "No AVI files found in directory %s", dir_path);
//         return ESP_FAIL;
//     }

//     avi_file_list = (char **)malloc(sizeof(char *) * count);
//     if (!avi_file_list)
//     {
//         closedir(dir);
//         ESP_LOGE(TAG, "Failed to allocate memory for file list");
//         return ESP_ERR_NO_MEM;
//     }
//     avi_file_count = count;
//     count = 0;

//     rewinddir(dir);
//     while ((entry = readdir(dir)) != NULL)
//     {
//         if (entry->d_type == DT_REG)
//         {
//             char *ext = strrchr(entry->d_name, '.');
//             if (ext && (strcasecmp(ext, ".avi") == 0))
//             {
//                 size_t dir_len = strlen(dir_path);
//                 size_t file_len = strlen(entry->d_name);
//                 char *full_path = (char *)malloc(dir_len + file_len + 2);
//                 if (!full_path)
//                 {
//                     ESP_LOGE(TAG, "Failed to allocate memory for file path");
//                     for (int i = 0; i < count; i++)
//                     {
//                         free(avi_file_list[i]);
//                     }
//                     free(avi_file_list);
//                     avi_file_list = NULL;
//                     avi_file_count = 0;
//                     closedir(dir);
//                     return ESP_ERR_NO_MEM;
//                 }

//                 if (dir_len > 0 && dir_path[dir_len - 1] == '/')
//                 {
//                     sprintf(full_path, "%s%s", dir_path, entry->d_name);
//                 }
//                 else
//                 {
//                     sprintf(full_path, "%s/%s", dir_path, entry->d_name);
//                 }

//                 avi_file_list[count++] = full_path;
//             }
//         }
//     }

//     closedir(dir);
//     ESP_LOGI(TAG, "Found %d AVI files in directory %s", avi_file_count, dir_path);
//     for (int i = 0; i < avi_file_count; i++)
//     {
//         ESP_LOGI(TAG, "AVI file %d: %s", i + 1, avi_file_list[i]);
//     }

//     return ESP_OK;
// }

static void init_canvas(void)
{
    if (canvas != NULL)
        return;
    if (!s_video_page || !lv_obj_is_valid(s_video_page))
    {
        ESP_LOGW("init_canvas", "video page not ready");
        return;
    }

    int w = frame_w > 0 ? frame_w : DISP_WIDTH;
    int h = frame_h > 0 ? frame_h : DISP_HEIGHT;

    for (int i = 0; i < 2; i++)
    {
        if (!canvas_buf[i])
        {
            canvas_buf[i] = (lv_color_t *)jpeg_calloc_align(w * h * sizeof(lv_color_t), 16);
            if (!canvas_buf[i])
            {
                ESP_LOGE("init_canvas", "alloc buf%d fail", i);
                // 简单释放已分配的
                for (int j = 0; j < i; j++)
                {
                    if (canvas_buf[j])
                    {
                        jpeg_free_align(canvas_buf[j]);
                        canvas_buf[j] = NULL;
                    }
                }
                return;
            }
        }
    }

    canvas = lv_canvas_create(s_video_page);
    lv_canvas_set_buffer(canvas, canvas_buf[0], w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas, w, h);
    lv_obj_center(canvas);
    lv_obj_move_foreground(canvas); // 确保在最上层
}

static esp_err_t init_jpeg_decoder(void)
{
    if (jpeg_handle != NULL)
    {
        return ESP_OK;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_error_t err = jpeg_dec_open(&config, &jpeg_handle);
    if (err != JPEG_ERR_OK)
    {
        ESP_LOGE("init_jpeg_decoder", "JPEG decoder initialization failed: %d", err);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void deinit_jpeg_decoder(void)
{
    if (jpeg_handle != NULL)
    {
        jpeg_dec_close(jpeg_handle);
        jpeg_handle = NULL;
    }
}

static void video_cb(frame_data_t *data, void *arg)
{
    static uint32_t frame_no = 0;
    if (s_video_stop_req)
        return;
    if (!data || !data->data || data->data_bytes == 0)
        return;

    if (init_jpeg_decoder() != ESP_OK)
        return;

    // 先解析 JPEG 头，拿到真实宽高
    jpeg_dec_io_t io = {
        .inbuf = data->data,
        .inbuf_len = data->data_bytes,
        .outbuf = NULL, // 稍后指定
    };
    jpeg_dec_header_info_t hi;
    jpeg_error_t err = jpeg_dec_parse_header(jpeg_handle, &io, &hi);
    if (err != JPEG_ERR_OK)
    {
        ESP_LOGE("video_cb", "parse hdr=%d", err);
        return;
    }

    int w = hi.width;
    int h = hi.height;

    // 需要时重建画布和双缓冲（帧尺寸变化或首次）
    bool need_recreate = (canvas == NULL) || (w != frame_w) || (h != frame_h);
    if (need_recreate)
    {
        // 释放旧画布与缓冲
        bsp_display_lock(0);
        if (canvas)
        {
            lv_obj_del(canvas);
            canvas = NULL;
        }
        bsp_display_unlock();
        for (int i = 0; i < 2; i++)
        {
            if (canvas_buf[i])
            {
                jpeg_free_align(canvas_buf[i]);
                canvas_buf[i] = NULL;
            }
        }
        frame_w = w;
        frame_h = h;
        current_buf_idx = 0;
        init_canvas(); // 重新用新尺寸建画布
        if (!canvas)
            return;
    }

    int next = (current_buf_idx + 1) % 2;

    int out_len = 0;
    err = jpeg_dec_get_outbuf_len(jpeg_handle, &out_len);
    if (err != JPEG_ERR_OK)
    {
        ESP_LOGE("video_cb", "get out len=%d", err);
        return;
    }
    int need = w * h * 2; // RGB565
    if (out_len > need)
    {
        ESP_LOGE("video_cb", "decoder reports %d > need %d (w=%d h=%d)", out_len, need, w, h);
        // 继续按 need 解码
    }

    // 真正把像素“解码到 next 缓冲”
    io.outbuf = (uint8_t *)canvas_buf[next];
    err = jpeg_dec_process(jpeg_handle, &io);
    if (err != JPEG_ERR_OK)
    {
        ESP_LOGE("video_cb", "decode=%d", err);
        return;
    }

    if (!s_video_page || !lv_obj_is_valid(s_video_page))
        return;

    // 切换显示到 next 缓冲
    bsp_display_lock(0);
    if (canvas)
    {
        if (need_recreate)
        {
            lv_canvas_set_buffer(canvas, canvas_buf[next], w, h, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_size(canvas, w, h);
            lv_obj_center(canvas);
        }
        else
        {
            lv_canvas_set_buffer(canvas, canvas_buf[next], frame_w, frame_h, LV_COLOR_FORMAT_RGB565);
        }
        current_buf_idx = next;
        lv_obj_invalidate(canvas);
    }
    bsp_display_unlock();

    if ((frame_no++ % 30) == 0)
    {
        ESP_LOGI("video_cb", "Frame %lu (%dx%d)", (unsigned long)frame_no, w, h);
    }
}

static void audio_cb(frame_data_t *data, void *arg)
{
    if (data && data->type == FRAME_TYPE_AUDIO && data->data && data->data_bytes > 0)
    {
        size_t bytes_written = 0;
        esp_err_t err = bsp_extra_i2s_write(data->data, data->data_bytes, &bytes_written, portMAX_DELAY);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Audio write failed: %s", esp_err_to_name(err));
        }
        else if (bytes_written != data->data_bytes)
        {
            ESP_LOGW(TAG, "Incomplete audio data (wrote %d/%d bytes)", bytes_written, data->data_bytes);
        }
    }
}

static void audio_set_clock_callback(uint32_t rate, uint32_t bits_cfg, uint32_t ch, void *arg)
{
    if (rate == 0)
    {
        rate = CODEC_DEFAULT_SAMPLE_RATE;
        ESP_LOGW(TAG, "Using default sample rate: %u", rate);
    }
    if (bits_cfg == 0)
    {
        bits_cfg = CODEC_DEFAULT_BIT_WIDTH;
        ESP_LOGW(TAG, "Using default bit width: %u", bits_cfg);
    }

    ESP_LOGI(TAG, "Setting I2S clock: sample rate=%u, bit width=%u, channels=%u", rate, bits_cfg, ch);
    i2s_slot_mode_t slot_mode = (ch == 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    esp_err_t err = bsp_extra_codec_set_fs(rate, bits_cfg, slot_mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set codec parameters: %s", esp_err_to_name(err));
    }
}

static void avi_end_cb(void *arg)
{
    ESP_LOGI(TAG, "AVI playback finished");
    is_playing = false;
}

static void avi_play_task(void *arg)
{
    s_video_task_exited = false;

    avi_player_handle_t handle;
    avi_player_config_t cfg = {
        .buffer_size = 256 * 1024,
        .video_cb = video_cb,
        .audio_cb = audio_cb,
        .audio_set_clock_cb = audio_set_clock_callback,
        .avi_play_end_cb = avi_end_cb,
        .priority = 7,
        .coreID = 0,
        .user_data = NULL,
        .stack_size = 12 * 1024,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .stack_in_psram = false,
#endif
    };

    // 先准备画布
    bsp_display_lock(0);
    init_canvas();
    bsp_display_unlock();

    ESP_ERROR_CHECK(avi_player_init(cfg, &handle));

    while (loop_playback && !s_video_stop_req)
    { // ← 关键改动
        if (avi_file_count <= 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (int i = 0; i < avi_file_count && loop_playback && !s_video_stop_req; i++)
        {
            const char *path = avi_file_list[i];

            avi_player_play_stop(handle);
            vTaskDelay(pdMS_TO_TICKS(50));

            if (s_video_stop_req)
                break;

            is_playing = true;
            esp_err_t err = avi_player_play_from_file(handle, path);
            if (err != ESP_OK)
            {
                is_playing = false;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            // 播放中：也要检查 stop
            while (loop_playback && is_playing && !s_video_stop_req)
            {
                vTaskDelay(pdMS_TO_TICKS(20));
                // ... 可选日志 ...
            }

            avi_player_play_stop(handle);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (loop_playback && !s_video_stop_req)
        {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

    // —— 收尾清理不变 —— //
    avi_player_play_stop(handle);
    avi_player_deinit(handle);
    deinit_jpeg_decoder();

    bsp_display_lock(0);
    for (int i = 0; i < 2; i++)
    {
        if (canvas_buf[i])
        {
            jpeg_free_align(canvas_buf[i]);
            canvas_buf[i] = NULL;
        }
    }
    if (canvas)
    {
        lv_obj_del(canvas);
        canvas = NULL;
    }
    bsp_display_unlock();

    if (avi_file_list)
    {
        for (int i = 0; i < avi_file_count; i++)
            free(avi_file_list[i]);
        free(avi_file_list);
        avi_file_list = NULL;
        avi_file_count = 0;
    }

    s_video_task_exited = true; // ← 告知 UI：可以切屏/删页了
    vTaskDelete(NULL);
}

void my_lv_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
    };

    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    // bsp_display_lock(0);
}

static void wait_exit_then_switch_cb(lv_timer_t *t)
{
    if (!s_video_task_exited)
        return; // 任务未退，继续等
    // 任务已退出：切回主页面，并自动删除“之前的页面”（即当前视频页）
    lv_obj_t *page = page_main_create();
    lv_scr_load_anim(page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
    s_video_page = NULL;
    lv_timer_del(t);
}

static void video_back_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    s_video_stop_req = true; // 1) 请求停止
    lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);

    // 2) 启轮询定时器，等播放任务完全退出再切屏
    lv_timer_t *tmr = lv_timer_create(wait_exit_then_switch_cb, 50, NULL);
    lv_timer_set_repeat_count(tmr, -1);
}

void video_audio_start(lv_obj_t *parent, char *path)
{
    ESP_ERROR_CHECK(bsp_extra_codec_init());
    bsp_extra_codec_volume_set(80, NULL);

    // 注意这里用 parent 作为父对象
    lv_obj_t *status_label = lv_label_create(parent);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label, DISP_WIDTH - 20);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);

    int retry_count = 0;
    esp_err_t mount_err = ESP_FAIL;
    while (1)
    {
        bsp_display_lock(0);
        lv_label_set_text_fmt(status_label, "Mounting SD card...\nAttempt: %d", retry_count + 1);
        bsp_display_unlock();

        mount_err = bsp_sdcard_mount();
        if (mount_err == ESP_OK)
        {
            break;
        }

        ESP_LOGW(TAG, "SD card mount attempt %d failed: %s", retry_count + 1, esp_err_to_name(mount_err));
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (mount_err != ESP_OK)
    {
        bsp_display_lock(0);
        lv_label_set_text(status_label, "SD card error\nCheck and restart");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        bsp_display_unlock();
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    esp_err_t list_err = get_avi_file_list(path);
    if (list_err != ESP_OK || avi_file_count == 0)
    {
        bsp_display_lock(0);
        lv_label_set_text(status_label, "No AVI files found\nin /sdcard/snr");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        bsp_display_unlock();
        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    bsp_display_lock(0);
    lv_obj_del(status_label);
    bsp_display_unlock();

    ESP_LOGI(TAG, "SD card mounted successfully, found %d AVI files", avi_file_count);

    xTaskCreatePinnedToCore(avi_play_task, "avi_play_task", 12288, NULL, 7, NULL, 0);
}

static void video_start_worker(void *arg)
{
    lv_obj_t *status_label = (lv_obj_t *)arg;

    // 音频编解码器初始化
    ESP_ERROR_CHECK(bsp_extra_codec_init());
    bsp_extra_codec_volume_set(80, NULL);

    // --- 挂载并提示 ---
    int retry = 0;
    esp_err_t mount_err = ESP_FAIL;
    while (!s_video_stop_req)
    {
        bsp_display_lock(0);
        lv_label_set_text_fmt(status_label, "Mounting SD card...\nAttempt: %d", ++retry);
        bsp_display_unlock();

        mount_err = bsp_sdcard_mount();
        if (mount_err == ESP_OK)
            break;

        ESP_LOGW(TAG, "SD mount attempt %d failed: %s", retry, esp_err_to_name(mount_err));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (s_video_stop_req)
        goto EXIT;

    if (mount_err != ESP_OK)
    {
        bsp_display_lock(0);
        lv_label_set_text(status_label, "SD card error\nCheck and restart");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        bsp_display_unlock();
        goto EXIT; // 不死循环，允许返回键退出
    }

    // --- 读取 AVI 列表 ---
    const char *dir = "/sdcard/am_nr"; // TODO: 如挂载点非 /sdcard，请改为实际 mount point
    esp_err_t list_err = get_avi_file_list(dir);
    if (list_err != ESP_OK || avi_file_count == 0)
    {
        bsp_display_lock(0);
        lv_label_set_text_fmt(status_label, "No AVI files found\nin %s", dir);
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        bsp_display_unlock();
        goto EXIT;
    }

    // 删除提示，开始播放
    bsp_display_lock(0);
    if (status_label)
        lv_obj_del(status_label);
    bsp_display_unlock();

    ESP_LOGI(TAG, "SD mounted, found %d AVI files", avi_file_count);

    // 把页面指针传给 avi_play_task（用于在该页面显示视频）
    // avi_play_task 内部循环里要定期检查 s_video_stop_req，收到后做善后并退出
    xTaskCreatePinnedToCore(avi_play_task, "avi_play_task", 12288, (void *)s_video_page, 7, NULL, 0);

EXIT:
    s_video_start_task = NULL;
    vTaskDelete(NULL);
}

// 你要的入口：在这里“创建新页面并播放”
void video_audio_start_on_new_page(void)
{
    // 1) 创建并切到新页面，拿到状态标签供后续提示
    lv_obj_t *status = create_video_page_and_get_status_label();

    // 2) 开后台任务做挂载/扫描/启动播放
    if (!s_video_start_task)
    {
        xTaskCreatePinnedToCore(video_start_worker, "video_start_worker", 4096,
                                (void *)status, 5, &s_video_start_task, 0);
    }
}