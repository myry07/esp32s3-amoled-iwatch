#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bsp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#if LVGL_VERSION_MAJOR >= 9
#define USE_LVGL_V9 1
#else
#define USE_LVGL_V9 0
#endif

#if USE_LVGL_V9
typedef struct { lv_image_dsc_t dsc; } dyn_img_v9_t;
#else
typedef struct { lv_img_dsc_t   dsc; } dyn_img_v8_t;
#endif

/* --- 删除时释放 pkg 的回调 --- */
static void img_free_on_delete(lv_event_t *e)
{
    void *pkg = lv_event_get_user_data(e);
    if(pkg) free(pkg);
}

/* 显示 JPG 为 lv_img/lv_image；视口大小 view_w x view_h；不缩放，小图居中，大图居中裁剪 */
lv_obj_t *show_jpg_as_img(lv_obj_t *parent, const char *jpg_path, int view_w, int view_h)
{
    if (!parent || !jpg_path || view_w <= 0 || view_h <= 0) return NULL;

    /* 1) 读文件 */
    FILE *fp = fopen(jpg_path, "rb");
    if (!fp) { printf("open %s failed\n", jpg_path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) { fclose(fp); printf("bad file size\n"); return NULL; }
    fseek(fp, 0, SEEK_SET);
    uint8_t *jpg_bytes = (uint8_t *)malloc((size_t)fsize);
    if (!jpg_bytes) { fclose(fp); printf("no mem jpg\n"); return NULL; }
    size_t rd = fread(jpg_bytes, 1, (size_t)fsize, fp);
    fclose(fp);
    if (rd != (size_t)fsize) { free(jpg_bytes); printf("read fail\n"); return NULL; }

    /* 2) 解码为 RGB565(LE) */
    jpeg_dec_handle_t j = NULL;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (jpeg_dec_open(&cfg, &j) != JPEG_ERR_OK) { free(jpg_bytes); printf("jpeg open fail\n"); return NULL; }

    jpeg_dec_io_t io = {.inbuf = jpg_bytes, .inbuf_len = (int)fsize, .outbuf = NULL};
    jpeg_dec_header_info_t hi;
    if (jpeg_dec_parse_header(j, &io, &hi) != JPEG_ERR_OK) {
        jpeg_dec_close(j); free(jpg_bytes); printf("parse header fail\n"); return NULL;
    }

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(j, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        jpeg_dec_close(j); free(jpg_bytes); printf("get out len fail\n"); return NULL;
    }

    uint8_t *rgb565 = (uint8_t *)jpeg_calloc_align((size_t)out_len, 16);
    if (!rgb565) { jpeg_dec_close(j); free(jpg_bytes); printf("no mem out\n"); return NULL; }
    io.outbuf = rgb565;

    if (jpeg_dec_process(j, &io) != JPEG_ERR_OK) {
        jpeg_dec_close(j); free(jpg_bytes); jpeg_free_align(rgb565); printf("decode fail\n"); return NULL;
    }

    const int img_w = (int)hi.width;
    const int img_h = (int)hi.height;

    /* 3) 计算居中裁剪窗口 */
    int copy_w = img_w, copy_h = img_h;
    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = 0, dst_y0 = 0;

    if (copy_w > view_w) { src_x0 = (copy_w - view_w) / 2; copy_w = view_w; }
    else                 { dst_x0 = (view_w - copy_w) / 2; }

    if (copy_h > view_h) { src_y0 = (copy_h - view_h) / 2; copy_h = view_h; }
    else                 { dst_y0 = (view_h - copy_h) / 2; }

    /* 4) 分配“一体化包”并拷入像素 */
    size_t dst_bytes = (size_t)view_w * view_h * 2; // RGB565
#if USE_LVGL_V9
    size_t pkg_bytes = sizeof(dyn_img_v9_t) + dst_bytes;
    dyn_img_v9_t *pkg = (dyn_img_v9_t *)malloc(pkg_bytes);
#else
    size_t pkg_bytes = sizeof(dyn_img_v8_t) + dst_bytes;
    dyn_img_v8_t *pkg = (dyn_img_v8_t *)malloc(pkg_bytes);
#endif
    if (!pkg) {
        jpeg_dec_close(j); free(jpg_bytes); jpeg_free_align(rgb565);
        printf("no mem pkg\n"); return NULL;
    }
    memset(pkg, 0, pkg_bytes);
    uint8_t *dst_pixels =
#if USE_LVGL_V9
        (uint8_t *)(pkg + 1);
#else
        (uint8_t *)(pkg + 1);
#endif
    memset(dst_pixels, 0, dst_bytes);

    for (int y = 0; y < copy_h; y++) {
        const uint8_t *src = rgb565 + ((size_t)(src_y0 + y) * img_w + src_x0) * 2;
        uint8_t *dst = dst_pixels + ((size_t)(dst_y0 + y) * view_w + dst_x0) * 2;
        memcpy(dst, src, (size_t)copy_w * 2);
    }

    /* 5) 填 dsc 头 */
#if USE_LVGL_V9
    pkg->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    pkg->dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
    pkg->dsc.header.flags = 0;
    pkg->dsc.header.w     = view_w;
    pkg->dsc.header.h     = view_h;
    pkg->dsc.data         = dst_pixels;
    pkg->dsc.data_size    = dst_bytes;
#else
    pkg->dsc.header.always_zero = 0;
    pkg->dsc.header.w           = view_w;
    pkg->dsc.header.h           = view_h;
    pkg->dsc.data               = dst_pixels;
    pkg->dsc.data_size          = dst_bytes;
    pkg->dsc.cf                 = LV_IMG_CF_TRUE_COLOR;
#endif

    /* 6) 创建对象 + 设置 src + 绑定删除回调(释放 pkg) */
    lv_obj_t *img = NULL;
    bsp_display_lock(portMAX_DELAY);
#if USE_LVGL_V9
    img = lv_image_create(parent);
    if (img) {
        lv_image_set_src(img, pkg);
        lv_obj_add_event_cb(img, img_free_on_delete, LV_EVENT_DELETE, pkg); // ★ 删除即释放
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    }
#else
    img = lv_img_create(parent);
    if (img) {
        lv_img_set_src(img, pkg);
        lv_obj_add_event_cb(img, img_free_on_delete, LV_EVENT_DELETE, pkg); // ★ 删除即释放
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    }
#endif
    bsp_display_unlock();

    /* 7) 释放临时缓冲 */
    jpeg_dec_close(j);
    free(jpg_bytes);
    jpeg_free_align(rgb565);

    return img;
}