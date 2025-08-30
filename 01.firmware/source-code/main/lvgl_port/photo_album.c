// album_page.c — 可直接替换

#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "esp_err.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "bsp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"   // 为了 bsp_display_lock/unlock
#include "ui.h"            // 如果里头没有声明 page_*，下面有 extern

/* 如若 UI 头文件没声明这两个函数，请保留这两行 extern */
extern lv_obj_t *page_lock_create(void);
extern lv_obj_t *page_main_create(void);

// ============================= 配置项 =============================
#define ALBUM_LOG(fmt, ...)  printf("[album] " fmt "\n", ##__VA_ARGS__)
#define JPEG_ALIGN           16   // esp_jpeg 对齐

// ========================== 内部状态/资源 =========================
typedef struct
{
    lv_obj_t    *page;         // 相册页面（容器）
    lv_obj_t    *canvas;       // 显示用 canvas
    lv_color_t  *canvas_buf;   // canvas 像素缓冲 (RGB565，对齐分配)

    int   cw, ch;              // canvas 尺寸
    bool  loop;                // 是否循环浏览

    // 解码缓冲（按需扩容，16B 对齐）
    uint8_t *decode_buf;
    int      decode_cap;

    // 文件列表
    char   **paths;
    int      count;
    int      index;

    // 手势
    bool         pressed;
    lv_point_t   p_down;

    // JPEG 解码器句柄（复用）
    jpeg_dec_handle_t j;
} album_ctx_t;

static album_ctx_t s_ctx = {0};

// ========================== 小工具函数 ============================
static void *safe_calloc_align(size_t n, size_t align)
{
    void *p = jpeg_calloc_align(n, align);
    if (!p) ALBUM_LOG("jpeg_calloc_align(%zu) failed", n);
    return p;
}
static void safe_free_align(void *p)
{
    if (p) jpeg_free_align(p);
}

static bool has_ext_icase(const char *name, const char *ext) // ext: ".jpg" / ".jpeg"
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    while (*ext && *dot) {
        char a = (char)tolower((unsigned char)*dot++);
        char b = (char)tolower((unsigned char)*ext++);
        if (a != b) return false;
    }
    return *dot == '\0' && *ext == '\0';
}

static void free_list(char **list, int n)
{
    if (!list) return;
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
}

// 扫描目录或接受单文件路径，收集 .jpg/.jpeg
static esp_err_t build_jpg_list(const char *path, char ***out_list, int *out_n)
{
    *out_list = NULL;
    *out_n    = 0;

    DIR *dir = opendir(path);
    if (!dir) {
        // 支持单文件路径
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (has_ext_icase(path, ".jpg") || has_ext_icase(path, ".jpeg"))) {
            char **one = (char**)calloc(1, sizeof(char*));
            if (!one) return ESP_ERR_NO_MEM;
            one[0] = strdup(path);
            if (!one[0]) { free(one); return ESP_ERR_NO_MEM; }
            *out_list = one;
            *out_n    = 1;
            return ESP_OK;
        }
        ALBUM_LOG("opendir(%s) failed and not a jpg file", path);
        return ESP_FAIL;
    }

    // 统计
    int cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (has_ext_icase(ent->d_name, ".jpg") || has_ext_icase(ent->d_name, ".jpeg")) cnt++;
    }
    if (cnt == 0) {
        closedir(dir);
        ALBUM_LOG("no jpg in %s", path);
        return ESP_FAIL;
    }

    char **list = (char**)calloc((size_t)cnt, sizeof(char*));
    if (!list) { closedir(dir); return ESP_ERR_NO_MEM; }

    rewinddir(dir);
    int idx = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!(has_ext_icase(ent->d_name, ".jpg") || has_ext_icase(ent->d_name, ".jpeg"))) continue;

        size_t dlen = strlen(path), flen = strlen(ent->d_name);
        bool has_sep = (dlen > 0 && (path[dlen-1] == '/' || path[dlen-1] == '\\'));
        size_t need = dlen + (has_sep ? 0 : 1) + flen + 1;

        char *full = (char*)malloc(need);
        if (!full) { closedir(dir); free_list(list, idx); return ESP_ERR_NO_MEM; }

        if (has_sep)  snprintf(full, need, "%s%s", path, ent->d_name);
        else          snprintf(full, need, "%s/%s", path, ent->d_name);

        // 快速 SOI 校验：不是 JPEG 就丢弃
        FILE *fp = fopen(full, "rb");
        bool ok = false;
        if (fp) {
            unsigned char soi[2] = {0};
            if (fread(soi, 1, 2, fp) == 2 && soi[0] == 0xFF && soi[1] == 0xD8) ok = true;
            fclose(fp);
        }
        if (!ok) { ESP_LOGW("album", "ignore non-jpeg: %s", full); free(full); continue; }

        list[idx++] = full;
    }
    closedir(dir);

    if (idx == 0) { free(list); ALBUM_LOG("no valid jpg in %s", path); return ESP_FAIL; }

    *out_list = list;
    *out_n    = idx; // 用实际数量
    return ESP_OK;
}

// ========================== 画布/显示 ============================

// 创建/复用 canvas（只在第一次创建）
static bool ensure_canvas(album_ctx_t *c)
{
    if (c->canvas) return true;

    c->canvas_buf = (lv_color_t*)safe_calloc_align((size_t)c->cw * c->ch * sizeof(lv_color_t), JPEG_ALIGN);
    if (!c->canvas_buf) { ALBUM_LOG("no mem canvas_buf"); return false; }

    bsp_display_lock(portMAX_DELAY);
    c->canvas = lv_canvas_create(c->page);
#if LVGL_VERSION_MAJOR >= 9
    lv_canvas_set_buffer(c->canvas, c->canvas_buf, c->cw, c->ch, LV_COLOR_FORMAT_RGB565);
#else
    lv_canvas_set_buffer(c->canvas, c->canvas_buf, c->cw, c->ch, LV_IMG_CF_TRUE_COLOR);
#endif
    lv_obj_center(c->canvas);
    bsp_display_unlock();

    return true;
}

// 把一帧 RGB565 居中贴到 canvas（大图居中裁剪，小图留黑边）
static void blit_center_rgb565(album_ctx_t *c, const uint8_t *rgb565, int img_w, int img_h)
{
    bsp_display_lock(portMAX_DELAY);

    // 清黑背景
    memset(c->canvas_buf, 0, (size_t)c->cw * c->ch * sizeof(lv_color_t));

    int copy_w = img_w, copy_h = img_h;
    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = 0, dst_y0 = 0;

    if (copy_w > c->cw) { src_x0 = (copy_w - c->cw) / 2; copy_w = c->cw; }
    else { dst_x0 = (c->cw - copy_w) / 2; }

    if (copy_h > c->ch) { src_y0 = (copy_h - c->ch) / 2; copy_h = c->ch; }
    else { dst_y0 = (c->ch - copy_h) / 2; }

    for (int y = 0; y < copy_h; y++) {
        const uint8_t *src_bytes = rgb565 + ((size_t)(src_y0 + y) * img_w + src_x0) * 2;
        uint8_t *dst_bytes = (uint8_t *)(c->canvas_buf + ((size_t)(dst_y0 + y) * c->cw + dst_x0));

    #if LV_COLOR_16_SWAP
        // 目标画布使用“交换后”的字节序：逐像素交换写入
        for (int x = 0; x < copy_w; x++) {
            uint8_t lo = src_bytes[2 * x + 0];  // LE: 低字节在前
            uint8_t hi = src_bytes[2 * x + 1];  // LE: 高字节在后
            // 画布需要 BE（或内存中交换后的序），把两个字节对调后写入
            dst_bytes[2 * x + 0] = hi;
            dst_bytes[2 * x + 1] = lo;
        }
    #else
        // 目标画布使用与解码相同的小端：直接搬
        memcpy(dst_bytes, src_bytes, (size_t)copy_w * 2);
    #endif
    }

    lv_obj_invalidate(c->canvas);
    bsp_display_unlock();
}


// =========================== JPEG 加载 ============================

static bool load_jpg(album_ctx_t *c, const char *path)
{
    // 删除旧的 img 对象
    if (c->canvas) {
        lv_obj_del(c->canvas);
        c->canvas = NULL;
    }

    // 直接用 show_jpg_as_img 创建一个新的 lv_img 对象
    lv_obj_t *img = show_jpg_as_img(c->page, path, c->cw, c->ch);
    if (!img) {
        ALBUM_LOG("load_jpg: show_jpg_as_img(%s) fail", path);
        return false;
    }

    c->canvas = img;  // 这里把 canvas 成员当成“当前显示的图片对象”保存
    return true;
}

// =========================== 事件回调 ============================

static void album_page_delete_cb(lv_event_t *e);  // 前置声明

static void album_event_cb(lv_event_t *e)
{
    static lv_point_t touch_start_point = {0};
    static bool gesture_detected = false;

    album_ctx_t *c = &s_ctx;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();

    switch (code) {

    case LV_EVENT_PRESSED: {
        gesture_detected = false;
        if (indev) lv_indev_get_point(indev, &touch_start_point);
        else touch_start_point.x = touch_start_point.y = 0;
        break;
    }

    case LV_EVENT_RELEASED: {
        if (!indev) break;

        lv_point_t touch_end_point = {0};
        lv_indev_get_point(indev, &touch_end_point);

        int dx = touch_end_point.x - touch_start_point.x;
        int dy = touch_end_point.y - touch_start_point.y;

        // 自适应阈值
        int thr = 15;
        lv_obj_t *target = lv_event_get_current_target(e);
        if (target) {
            int w = lv_obj_get_width(target);
            if (w / 20 > thr) thr = w / 20;     // ~5%
        }
        uint32_t dpi = lv_disp_get_dpi(NULL);
        if ((int)(dpi / 8) > thr) thr = (int)(dpi / 8);
        if (thr < 12) thr = 12;
        if (thr > 60) thr = 60;

        if (abs(dx) < thr && abs(dy) < thr) break; // 位移小：非滑动

        gesture_detected = true;

        if (abs(dx) >= abs(dy)) {
            // 左右滑切换图片
            if (!c || c->count <= 0 || !c->paths) break;

            int next = c->index;
            if (dx > 0) { // 右滑
                ESP_LOGI("gesture", "右滑");
                next = c->index - 1;
                if (next < 0) next = c->loop ? (c->count - 1) : 0;
            } else {       // 左滑
                ESP_LOGI("gesture", "左滑");
                next = c->index + 1;
                if (next >= c->count) next = c->loop ? 0 : (c->count - 1);
            }

            if (next != c->index) {
                c->index = next;
                (void)load_jpg(c, c->paths[c->index]);
            }
        } else {
            // 上/下滑切屏
            if (dy > thr) {
                ESP_LOGI("gesture", "下滑");
                lv_obj_t *new_scr = page_lock_create();
                if (new_scr) lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 120, 0, true);
            } else {
                ESP_LOGI("gesture", "上滑");
                lv_obj_t *new_scr = page_main_create();
                if (new_scr) lv_scr_load_anim(new_scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 120, 0, true);
            }
        }
        break;
    }

    case LV_EVENT_CLICKED:
        if (gesture_detected) break; // 刚识别为滑动则不当点击
        ESP_LOGI("btn", "点击事件");
        break;

    default:
        break;
    }
}

// =========================== 对外接口 ============================

// 创建相册页面（dir 可为目录或单文件 .jpg/.jpeg）
lv_obj_t *photo_album_create(const char *dir, int canvas_w, int canvas_h, bool loop)
{
    album_ctx_t *c = &s_ctx;

    // 清理旧实例（若存在）
    if (c->canvas) {
        if (c->canvas_buf) { safe_free_align(c->canvas_buf); c->canvas_buf = NULL; }
        lv_obj_del(c->canvas);
        c->canvas = NULL;
    }
    if (c->page)   { lv_obj_del(c->page); c->page = NULL; }
    if (c->decode_buf) { safe_free_align(c->decode_buf); c->decode_buf = NULL; c->decode_cap = 0; }
    if (c->paths)  { free_list(c->paths, c->count); c->paths = NULL; c->count = 0; }
    if (c->j)      { jpeg_dec_close(c->j); c->j = NULL; }

    c->cw   = canvas_w;
    c->ch   = canvas_h;
    c->loop = loop;

    if (build_jpg_list(dir, &c->paths, &c->count) != ESP_OK) {
        ALBUM_LOG("build list fail: %s", dir);
        return NULL;
    }

    c->index = 0;

    // 页面容器
    c->page = lv_obj_create(NULL);
    lv_obj_set_size(c->page, canvas_w, canvas_h);
    lv_obj_set_style_bg_opa(c->page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(c->page, LV_OBJ_FLAG_SCROLLABLE);

    // 事件绑定
    lv_obj_add_event_cb(c->page, album_event_cb,      LV_EVENT_ALL,    NULL);
    lv_obj_add_event_cb(c->page, album_page_delete_cb,LV_EVENT_DELETE,  NULL);

    // 首张
    (void)load_jpg(c, c->paths[c->index]);

    return c->page;
}

// 销毁相册（主动删）
void photo_album_destroy(void)
{
    album_ctx_t *c = &s_ctx;

    if (c->canvas) {
        if (c->canvas_buf) { safe_free_align(c->canvas_buf); c->canvas_buf = NULL; }
        lv_obj_del(c->canvas);
        c->canvas = NULL;
    }
    if (c->page)   { lv_obj_del(c->page); c->page = NULL; }
    if (c->decode_buf) { safe_free_align(c->decode_buf); c->decode_buf = NULL; c->decode_cap = 0; }
    if (c->paths)  { free_list(c->paths, c->count); c->paths = NULL; c->count = 0; }
    if (c->j)      { jpeg_dec_close(c->j); c->j = NULL; }
}

// 仅释放我们分配的资源（让 LVGL 自己删对象）
static void album_free_resources_only(void)
{
    album_ctx_t *c = &s_ctx;

    if (c->canvas_buf) { safe_free_align(c->canvas_buf); c->canvas_buf = NULL; }
    if (c->decode_buf) { safe_free_align(c->decode_buf); c->decode_buf = NULL; c->decode_cap = 0; }
    if (c->paths)      { free_list(c->paths, c->count); c->paths = NULL; c->count = 0; }
    if (c->j)          { jpeg_dec_close(c->j); c->j = NULL; }
    // canvas/page 交给 LVGL；DELETE 后它们自然无效
}

// 页面删除回调：释放我们分配的资源并清空指针
static void album_page_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    album_free_resources_only();

    s_ctx.canvas = NULL;
    s_ctx.page   = NULL;
}