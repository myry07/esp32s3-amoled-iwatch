#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "bsp.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*--- 辅助宏：判断 LVGL 版本 ---*/
#if LVGL_VERSION_MAJOR >= 9
#define USE_LVGL_V9 1
#else
#define USE_LVGL_V9 0
#endif

/*--- 解码后作为 lv_img 源的数据结构（包含 LVGL 头 + 像素数据） ---*/
#if USE_LVGL_V9
typedef struct
{
    lv_image_dsc_t dsc; // v9 的图片描述
    // 后面紧跟像素数据
} dyn_img_v9_t;
#else
typedef struct
{
    lv_img_dsc_t dsc; // v8 的图片描述
    // 后面紧跟像素数据
} dyn_img_v8_t;
#endif

/* 释放函数：当不再需要该图片时，调用它释放内存 */
void free_dynamic_img(void *img_src)
{
#if USE_LVGL_V9
    dyn_img_v9_t *pkg = (dyn_img_v9_t *)img_src;
    if (pkg)
        free(pkg);
#else
    dyn_img_v8_t *pkg = (dyn_img_v8_t *)img_src;
    if (pkg)
        free(pkg);
#endif
}

/* 把 JPG 文件显示为 lv_img（不使用 canvas），宽高不缩放：小图居中，大图居中裁剪 */
lv_obj_t *show_jpg_as_img(lv_obj_t *parent, const char *jpg_path, int view_w, int view_h)
{
    if (!parent || !jpg_path || view_w <= 0 || view_h <= 0)
        return NULL;

    // 1) 读取文件
    FILE *fp = fopen(jpg_path, "rb");
    if (!fp)
    {
        printf("open %s failed\n", jpg_path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0)
    {
        fclose(fp);
        printf("bad file size\n");
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    uint8_t *jpg_bytes = (uint8_t *)malloc((size_t)fsize);
    if (!jpg_bytes)
    {
        fclose(fp);
        printf("no mem jpg\n");
        return NULL;
    }
    size_t rd = fread(jpg_bytes, 1, (size_t)fsize, fp);
    fclose(fp);
    if (rd != (size_t)fsize)
    {
        free(jpg_bytes);
        printf("read fail\n");
        return NULL;
    }

    // 2) JPEG 解码为 RGB565（LE）
    jpeg_dec_handle_t j = NULL;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE; // 与你的视频路径一致
    if (jpeg_dec_open(&cfg, &j) != JPEG_ERR_OK)
    {
        free(jpg_bytes);
        printf("jpeg open fail\n");
        return NULL;
    }

    jpeg_dec_io_t io = {.inbuf = jpg_bytes, .inbuf_len = (int)fsize, .outbuf = NULL};
    jpeg_dec_header_info_t hi;
    if (jpeg_dec_parse_header(j, &io, &hi) != JPEG_ERR_OK)
    {
        jpeg_dec_close(j);
        free(jpg_bytes);
        printf("parse header fail\n");
        return NULL;
    }

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(j, &out_len) != JPEG_ERR_OK || out_len <= 0)
    {
        jpeg_dec_close(j);
        free(jpg_bytes);
        printf("get out len fail\n");
        return NULL;
    }

    uint8_t *rgb565 = (uint8_t *)jpeg_calloc_align((size_t)out_len, 16);
    if (!rgb565)
    {
        jpeg_dec_close(j);
        free(jpg_bytes);
        printf("no mem out\n");
        return NULL;
    }
    io.outbuf = rgb565;

    if (jpeg_dec_process(j, &io) != JPEG_ERR_OK)
    {
        jpeg_dec_close(j);
        free(jpg_bytes);
        jpeg_free_align(rgb565);
        printf("decode fail\n");
        return NULL;
    }

    const int img_w = (int)hi.width;
    const int img_h = (int)hi.height;

    // 3) 计算居中裁剪/贴图窗口（不缩放）
    int copy_w = img_w, copy_h = img_h;
    int src_x0 = 0, src_y0 = 0;
    int dst_x0 = 0, dst_y0 = 0;

    if (copy_w > view_w)
    {
        src_x0 = (copy_w - view_w) / 2;
        copy_w = view_w;
    }
    else
    {
        dst_x0 = (view_w - copy_w) / 2;
    }

    if (copy_h > view_h)
    {
        src_y0 = (copy_h - view_h) / 2;
        copy_h = view_h;
    }
    else
    {
        dst_y0 = (view_h - copy_h) / 2;
    }

    // 4) 申请“LVGL 图片 + 像素”的一体化内存块，并把裁剪后的像素拷入
    size_t dst_bytes = (size_t)view_w * view_h * 2; // RGB565
#if USE_LVGL_V9
    size_t pkg_bytes = sizeof(dyn_img_v9_t) + dst_bytes;
    dyn_img_v9_t *pkg = (dyn_img_v9_t *)malloc(pkg_bytes);
    if (!pkg)
    {
        jpeg_dec_close(j);
        free(jpg_bytes);
        free(rgb565);
        printf("no mem pkg\n");
        return NULL;
    }
    memset(pkg, 0, sizeof(*pkg));
    uint8_t *dst_pixels = (uint8_t *)(pkg + 1);

    // 填充 v9 头
    pkg->dsc.header.magic = LV_IMAGE_HEADER_MAGIC; // 必填魔数
    pkg->dsc.header.cf = LV_COLOR_FORMAT_RGB565;   // 与显示一致
    pkg->dsc.header.flags = 0;
    pkg->dsc.header.w = view_w;
    pkg->dsc.header.h = view_h;
    pkg->dsc.data = dst_pixels;
    pkg->dsc.data_size = dst_bytes;
#else
    size_t pkg_bytes = sizeof(dyn_img_v8_t) + dst_bytes;
    dyn_img_v8_t *pkg = (dyn_img_v8_t *)malloc(pkg_bytes);
    if (!pkg)
    {
        jpeg_dec_close(j);
        free(jpg_bytes);
        free(rgb565);
        printf("no mem pkg\n");
        return NULL;
    }
    memset(pkg, 0, sizeof(*pkg));
    uint8_t *dst_pixels = (uint8_t *)(pkg + 1);

    // 填充 v8 头
    pkg->dsc.header.always_zero = 0;
    pkg->dsc.header.w = view_w;
    pkg->dsc.header.h = view_h;
    pkg->dsc.data_size = dst_bytes;
    pkg->dsc.data = dst_pixels;
    pkg->dsc.cf = LV_IMG_CF_TRUE_COLOR; // v8 用 TRUE_COLOR
#endif

    // 5) 把 RGB565（不缩放）拷入目标图像缓冲：小图居中/大图居中裁剪
    // 先清空，避免留痕
    memset(dst_pixels, 0, dst_bytes);

    for (int y = 0; y < copy_h; y++)
    {
        const uint8_t *src = rgb565 + ((size_t)(src_y0 + y) * img_w + src_x0) * 2;
        uint8_t *dst = dst_pixels + ((size_t)(dst_y0 + y) * view_w + dst_x0) * 2;
        memcpy(dst, src, (size_t)copy_w * 2);
    }

    // 6) 生成 lv_img 对象并设置 src
    lv_obj_t *img = NULL;
    bsp_display_lock(portMAX_DELAY);
    img = lv_img_create(parent);
    if (img)
    {
#if USE_LVGL_V9
        lv_image_set_src(img, pkg);
#else
        lv_img_set_src(img, pkg);
#endif
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    }
    bsp_display_unlock();

    // 7) 清理临时资源（注意：pkg 要在不用时手动 free）
    jpeg_dec_close(j);
    free(jpg_bytes);
    free(rgb565);

    return img;
}

/* 用完之后释放：
   free_dynamic_img(lv_img_get_src(img));    // v8
   free_dynamic_img(lv_image_get_src(img));  // v9
*/