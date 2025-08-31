#pragma once
#include "lvgl.h"

#if defined(LV_100ASK_2048_MATRIX_SIZE)
#define MATRIX_SIZE LV_100ASK_2048_MATRIX_SIZE
#else
#define MATRIX_SIZE 4
#endif

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *btnm;
    uint32_t score;
    uint16_t map_count;
    uint16_t matrix[MATRIX_SIZE][MATRIX_SIZE];
    const char *btnm_map[MATRIX_SIZE * (MATRIX_SIZE + 1) + 1];
    bool game_over;
    lv_point_t touch_start;
    bool touching;
} lv_100ask_2048_t;

lv_obj_t *lv_100ask_2048_create(lv_obj_t *parent);
void lv_100ask_2048_set_new_game(lv_obj_t *root);
uint32_t lv_100ask_2048_get_score(lv_obj_t *root);
bool lv_100ask_2048_get_status(lv_obj_t *root);
uint16_t lv_100ask_2048_get_best_tile(lv_obj_t *root);