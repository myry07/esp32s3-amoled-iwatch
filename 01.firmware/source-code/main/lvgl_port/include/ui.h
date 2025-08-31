#ifndef UI_H_
#define UI_H_

#include "lvgl.h"

void my_lv_start(void);

void solid_test(void);

lv_obj_t *show_jpg_as_img(lv_obj_t *parent, const char *jpg_path, int view_w, int view_h);

lv_obj_t *photo_album_create(const char *dir, int canvas_w, int canvas_h, bool loop);

lv_obj_t *page_lock_create(void);
lv_obj_t *page_main_create(void);
lv_obj_t *page1_create(void);

lv_obj_t *settings_page(void);

void video_audio_start(lv_obj_t *parent, char *path);
void video_audio_start_on_new_page(void);

#endif