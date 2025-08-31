/**
 * @file lv_100ask_2048.c  (LVGL9 版，user_data 状态，无自定义 class)
 */

#include "game1.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/*********************
 *      DEFINES
 *********************/
#define LV_100ASK_2048_TEXT_BLACK_COLOR lv_color_hex(0x6c635b)
#define LV_100ASK_2048_TEXT_WHITE_COLOR lv_color_hex(0xf8f5f0)

#define LV_100ASK_2048_NUMBER_EMPTY_COLOR lv_color_hex(0xc7b9ac)
#define LV_100ASK_2048_NUMBER_2_COLOR lv_color_hex(0xeee4da)
#define LV_100ASK_2048_NUMBER_4_COLOR lv_color_hex(0xede0c8)
#define LV_100ASK_2048_NUMBER_8_COLOR lv_color_hex(0xf2b179)
#define LV_100ASK_2048_NUMBER_16_COLOR lv_color_hex(0xf59563)
#define LV_100ASK_2048_NUMBER_32_COLOR lv_color_hex(0xf67c5f)
#define LV_100ASK_2048_NUMBER_64_COLOR lv_color_hex(0xf75f3b)
#define LV_100ASK_2048_NUMBER_128_COLOR lv_color_hex(0xedcf72)
#define LV_100ASK_2048_NUMBER_256_COLOR lv_color_hex(0xedcc61)
#define LV_100ASK_2048_NUMBER_512_COLOR lv_color_hex(0xedc850)
#define LV_100ASK_2048_NUMBER_1024_COLOR lv_color_hex(0xedc53f)
#define LV_100ASK_2048_NUMBER_2048_COLOR lv_color_hex(0xedc22e)

/**********************
 *  FORWARD DECLS
 **********************/
typedef lv_100ask_2048_t state_t;

static state_t *get_state(lv_obj_t *root);
static void free_state_cb(lv_event_t *e);
static void btnm_event_cb(lv_event_t *e);
static void game_play_event(lv_event_t *e);

static void init_matrix_num(uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static void addRandom(uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static void update_btnm_map(state_t *s);
static lv_color_t get_num_color(uint16_t num);

static uint8_t count_empty(uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static uint8_t find_target(uint16_t array[MATRIX_SIZE], uint8_t x, uint8_t stop);
static void rotate_matrix(uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static bool find_pair_down(uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static bool slide_array(uint32_t *score, uint16_t array[MATRIX_SIZE]);
static bool move_up(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static bool move_down(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static bool move_left(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static bool move_right(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);
static bool game_over(uint16_t m[MATRIX_SIZE][MATRIX_SIZE]);

/**********************
 *   INTERNAL HELPERS
 **********************/
static state_t *get_state(lv_obj_t *root)
{
    return (state_t *)lv_obj_get_user_data(root);
}

static void free_state_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE)
        return;
    lv_obj_t *root = lv_event_get_target(e);
    state_t *s = get_state(root);
    if (!s)
        return;

    for (uint16_t i = 0; i < s->map_count; i++)
    {
        const char *p = s->btnm_map[i];
        if (!p)
            continue;
        if (p[0] == '\n' || p[0] == '\0')
            continue; // 非动态块
        lv_free((void *)p);
    }
    lv_free(s);
}

/**********************
 *   PUBLIC API
 **********************/

lv_obj_t *lv_100ask_2048_create(lv_obj_t *parent)
{
    // 根容器
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);

    // 分配状态
    state_t *s = (state_t *)lv_malloc(sizeof(*s));
    LV_ASSERT_MALLOC(s);
    memset(s, 0, sizeof(*s));
    s->root = root;
    s->map_count = MATRIX_SIZE * (MATRIX_SIZE + 1) + 1; // N 列 + 每行换行 + 末尾 ""

    // 分配/填充 map（const char*）
    for (uint16_t i = 0; i < s->map_count; i++)
    {
        bool is_last = (i + 1 == s->map_count);
        bool is_newline = !is_last && ((i + 1) % (MATRIX_SIZE + 1) == 0);
        if (is_last)
            s->btnm_map[i] = "";
        else if (is_newline)
            s->btnm_map[i] = "\n";
        else
        {
            char *buf = lv_malloc(10); // 够显示 "32768\0"
            buf[0] = ' ';
            buf[1] = '\0';
            s->btnm_map[i] = buf;
        }
    }

    lv_obj_set_user_data(root, s);
    lv_obj_add_event_cb(root, free_state_cb, LV_EVENT_DELETE, NULL);

    // 创建 button matrix
    s->btnm = lv_btnmatrix_create(root);
    lv_obj_set_size(s->btnm, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s->btnm);
    lv_obj_set_style_pad_all(s->btnm, 10, 0);
    lv_obj_set_style_border_width(s->btnm, 0, 0);

    lv_obj_set_style_text_font(s->btnm, &lv_font_montserrat_24, LV_PART_ITEMS);

    /* ★ 必须：启用绘制任务事件（否则 btnm_event_cb 不会跑） */
    lv_obj_add_flag(s->btnm, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);

    /* 可选：先给一个底色，便于看出区域 */
    lv_obj_set_style_bg_color(s->btnm, lv_color_hex(0xb3a397), 0);

    /* v9 绘制事件 + 游戏控制 */
    lv_obj_add_event_cb(s->btnm, btnm_event_cb, LV_EVENT_DRAW_TASK_ADDED, root);
    lv_obj_add_event_cb(s->btnm, game_play_event, LV_EVENT_ALL, root);

    /* 初始化地图并 set_map（你已有） */
    lv_100ask_2048_set_new_game(root);

    return root;
}

void lv_100ask_2048_set_new_game(lv_obj_t *root)
{
    state_t *s = get_state(root);

    s->score = 0;
    s->game_over = false;

    init_matrix_num(s->matrix);
    update_btnm_map(s);

    lv_btnmatrix_set_map(s->btnm, s->btnm_map);
    lv_obj_send_event(root, LV_EVENT_VALUE_CHANGED, NULL);
}

uint32_t lv_100ask_2048_get_score(lv_obj_t *root)
{
    return get_state(root)->score;
}

bool lv_100ask_2048_get_status(lv_obj_t *root)
{
    return get_state(root)->game_over;
}

uint16_t lv_100ask_2048_get_best_tile(lv_obj_t *root)
{
    state_t *s = get_state(root);
    uint8_t x, y;
    uint16_t best_tile = 0;

    for (x = 0; x < MATRIX_SIZE; x++)
    {
        for (y = 0; y < MATRIX_SIZE; y++)
        {
            if (best_tile < s->matrix[x][y])
                best_tile = s->matrix[x][y];
        }
    }
    return (uint16_t)(1u << best_tile);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void game_play_event(lv_event_t *e)
{
    lv_obj_t *root = lv_event_get_user_data(e);
    state_t *s = get_state(root);
    lv_event_code_t code = lv_event_get_code(e);
    bool success = false;

    /* ---- 键盘方向（保留） ---- */
    if (code == LV_EVENT_KEY)
    {
        uint32_t key = lv_event_get_key(e);
        switch (key)
        {
        case LV_KEY_UP:
            success = move_left(&(s->score), s->matrix);
            break;
        case LV_KEY_DOWN:
            success = move_right(&(s->score), s->matrix);
            break;
        case LV_KEY_LEFT:
            success = move_up(&(s->score), s->matrix);
            break;
        case LV_KEY_RIGHT:
            success = move_down(&(s->score), s->matrix);
            break;
        default:
            break;
        }
    }

    /* ---- 软手势：按下记录，抬起判断 ---- */
    else if (code == LV_EVENT_PRESSED)
    {
        s->touching = true;
        lv_indev_t *indev = lv_indev_active(); // v9 推荐这个
        if (indev)
            lv_indev_get_point(indev, &s->touch_start);
        return;
    }
    else if (code == LV_EVENT_RELEASED && s->touching)
    {
        s->touching = false;

        lv_point_t end = {0, 0};
        lv_indev_t *indev = lv_indev_active();
        if (indev)
            lv_indev_get_point(indev, &end);

        int dx = end.x - s->touch_start.x;
        int dy = end.y - s->touch_start.y;

        const int TH = 20; // 阈值，必要时调大点
        if (LV_ABS(dx) < TH && LV_ABS(dy) < TH)
            return;

        /* 根据你棋盘朝向做的映射（与此前保持一致） */
        if (LV_ABS(dx) > LV_ABS(dy))
        {
            success = (dx > 0) ? move_down(&(s->score), s->matrix)
                               : move_up(&(s->score), s->matrix);
        }
        else
        {
            success = (dy > 0) ? move_right(&(s->score), s->matrix)
                               : move_left(&(s->score), s->matrix);
        }
    }

    /* 有动作就落子+刷新 */
    if (success)
    {
        if (!game_over(s->matrix))
        {
            addRandom(s->matrix);
        }
        else
        {
            LV_LOG_USER("100ASK 2048 GAME OVER!");
        }
        update_btnm_map(s);
        lv_btnmatrix_set_map(s->btnm, s->btnm_map);
        lv_obj_send_event(root, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

static void btnm_event_cb(lv_event_t *e)
{
    lv_obj_t *root = lv_event_get_user_data(e);
    state_t *s = get_state(root);

    /* 取得本次绘制的 draw task */
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    if (!draw_task)
        return;

    /* v9 正确的 base 描述符获取方式 */
    lv_draw_dsc_base_t *base_dsc = lv_draw_task_get_draw_dsc(draw_task);
    if (!base_dsc)
        return;

    /* 只处理按钮矩阵的 item 部件 */
    if (base_dsc->obj == s->btnm && base_dsc->part == LV_PART_ITEMS)
    {

        /* 各子类型的 dsc：文字、填充、边框 */
        lv_draw_label_dsc_t *label_dsc = lv_draw_task_get_label_dsc(draw_task);
        lv_draw_fill_dsc_t *fill_dsc = lv_draw_task_get_fill_dsc(draw_task);
        lv_draw_border_dsc_t *border_dsc = lv_draw_task_get_border_dsc(draw_task);

        /* btnmatrix 的按钮索引在 id1 */
        uint32_t id = base_dsc->id1;
        uint16_t x = (uint16_t)(id / MATRIX_SIZE);
        uint16_t y = (uint16_t)(id % MATRIX_SIZE);
        if (x < MATRIX_SIZE && y < MATRIX_SIZE)
        {
            uint16_t num = (uint16_t)(1u << s->matrix[x][y]);

            if (fill_dsc)
            {
                fill_dsc->radius = 3;
                fill_dsc->color = get_num_color(num);
            }
            if (border_dsc)
            {
                border_dsc->width = 0; /* 去掉边框 */
            }
            if (label_dsc)
            {
                label_dsc->color = (num < 8) ? LV_100ASK_2048_TEXT_BLACK_COLOR
                                             : LV_100ASK_2048_TEXT_WHITE_COLOR;
            }
        }
    }
}

static void init_matrix_num(uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    for (uint8_t x = 0; x < MATRIX_SIZE; x++)
        for (uint8_t y = 0; y < MATRIX_SIZE; y++)
            m[x][y] = 0;

    addRandom(m);
    addRandom(m);
}

static void addRandom(uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    uint16_t x, y, len = 0;
    uint16_t list[MATRIX_SIZE * MATRIX_SIZE][2];

    for (x = 0; x < MATRIX_SIZE; x++)
    {
        for (y = 0; y < MATRIX_SIZE; y++)
        {
            if (m[x][y] == 0)
            {
                list[len][0] = x;
                list[len][1] = y;
                len++;
            }
        }
    }

    if (len > 0)
    {
        uint16_t r = (uint16_t)lv_rand(0, len - 1);
        x = list[r][0];
        y = list[r][1];
        /* 90% 出 2(=1<<1)，10% 出 4(=1<<2) */
        m[x][y] = (lv_rand(0, 9) == 9) ? 2 : 1;
    }
}

static void update_btnm_map(state_t *s)
{
    uint16_t idx = 0;

    for (uint8_t x = 0; x < MATRIX_SIZE; x++)
    {
        for (uint8_t y = 0; y < MATRIX_SIZE; y++)
        {

            if (((idx + 1) % (MATRIX_SIZE + 1)) == 0)
                idx++; // 跳过换行槽

            if (s->matrix[x][y] != 0)
            {
                // 可写缓冲长度 10
                snprintf((char *)s->btnm_map[idx], 10, "%u", (unsigned)(1u << s->matrix[x][y]));
            }
            else
            {
                ((char *)s->btnm_map[idx])[0] = ' ';
                ((char *)s->btnm_map[idx])[1] = '\0';
            }
            idx++;
        }
        if (((idx + 1) % (MATRIX_SIZE + 1)) == 0)
            idx++; // 行尾再跳过换行槽
    }
}

static uint8_t find_target(uint16_t array[MATRIX_SIZE], uint8_t x, uint8_t stop)
{
    if (x == 0)
        return x;
    for (uint8_t t = x - 1;; t--)
    {
        if (array[t] != 0)
        {
            if (array[t] != array[x])
                return t + 1;
            return t;
        }
        else
        {
            if (t == stop)
                return t;
        }
    }
    return x;
}

static bool slide_array(uint32_t *score, uint16_t array[MATRIX_SIZE])
{
    bool success = false;
    uint8_t stop = 0;

    for (uint8_t x = 0; x < MATRIX_SIZE; x++)
    {
        if (array[x] != 0)
        {
            uint8_t t = find_target(array, x, stop);
            if (t != x)
            {
                if (array[t] == 0)
                {
                    array[t] = array[x];
                }
                else if (array[t] == array[x])
                {
                    array[t]++;
                    *score += (uint32_t)1u << array[t];
                    stop = t + 1;
                }
                array[x] = 0;
                success = true;
            }
        }
    }
    return success;
}

static void rotate_matrix(uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    uint8_t n = MATRIX_SIZE;
    for (uint8_t i = 0; i < (n / 2); i++)
    {
        for (uint8_t j = i; j < (n - i - 1); j++)
        {
            uint16_t tmp = m[i][j];
            m[i][j] = m[j][n - i - 1];
            m[j][n - i - 1] = m[n - i - 1][n - j - 1];
            m[n - i - 1][n - j - 1] = m[n - j - 1][i];
            m[n - j - 1][i] = tmp;
        }
    }
}

static bool move_up(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    bool success = false;
    for (uint8_t x = 0; x < MATRIX_SIZE; x++)
        success |= slide_array(score, m[x]);
    return success;
}

static bool move_left(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    rotate_matrix(m);
    bool success = move_up(score, m);
    rotate_matrix(m);
    rotate_matrix(m);
    rotate_matrix(m);
    return success;
}

static bool move_down(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    rotate_matrix(m);
    rotate_matrix(m);
    bool success = move_up(score, m);
    rotate_matrix(m);
    rotate_matrix(m);
    return success;
}

static bool move_right(uint32_t *score, uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    rotate_matrix(m);
    rotate_matrix(m);
    rotate_matrix(m);
    bool success = move_up(score, m);
    rotate_matrix(m);
    return success;
}

static bool find_pair_down(uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    for (uint8_t x = 0; x < MATRIX_SIZE; x++)
    {
        for (uint8_t y = 0; y < (MATRIX_SIZE - 1); y++)
        {
            if (m[x][y] == m[x][y + 1])
                return true;
        }
    }
    return false;
}

static uint8_t count_empty(uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    uint8_t cnt = 0;
    for (uint8_t x = 0; x < MATRIX_SIZE; x++)
        for (uint8_t y = 0; y < MATRIX_SIZE; y++)
            if (m[x][y] == 0)
                cnt++;
    return cnt;
}

static bool game_over(uint16_t m[MATRIX_SIZE][MATRIX_SIZE])
{
    if (count_empty(m) > 0)
        return false;
    if (find_pair_down(m))
        return false;

    rotate_matrix(m);
    bool ended = !find_pair_down(m);
    rotate_matrix(m);
    rotate_matrix(m);
    rotate_matrix(m);

    return ended;
}

static lv_color_t get_num_color(uint16_t num)
{
    switch (num)
    {
    case 0:
        return LV_100ASK_2048_NUMBER_EMPTY_COLOR;
    case 1:
        return LV_100ASK_2048_NUMBER_EMPTY_COLOR; // 空格(1<<0)显示为空色
    case 2:
        return LV_100ASK_2048_NUMBER_2_COLOR;
    case 4:
        return LV_100ASK_2048_NUMBER_4_COLOR;
    case 8:
        return LV_100ASK_2048_NUMBER_8_COLOR;
    case 16:
        return LV_100ASK_2048_NUMBER_16_COLOR;
    case 32:
        return LV_100ASK_2048_NUMBER_32_COLOR;
    case 64:
        return LV_100ASK_2048_NUMBER_64_COLOR;
    case 128:
        return LV_100ASK_2048_NUMBER_128_COLOR;
    case 256:
        return LV_100ASK_2048_NUMBER_256_COLOR;
    case 512:
        return LV_100ASK_2048_NUMBER_512_COLOR;
    case 1024:
        return LV_100ASK_2048_NUMBER_1024_COLOR;
    case 2048:
        return LV_100ASK_2048_NUMBER_2048_COLOR;
    default:
        return LV_100ASK_2048_NUMBER_2048_COLOR;
    }
}