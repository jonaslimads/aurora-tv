#include "notice.h"

#include "lvgl.h"

/* Kept between calls: the box is created once and reused, and a recreated display
 * invalidates it, which is why every use re-checks it instead of trusting the
 * pointer. */
static lv_obj_t *notice_box = NULL;
static lv_obj_t *notice_label = NULL;
static lv_timer_t *notice_timer = NULL;

static void notice_hide_cb(lv_timer_t *timer);

void ui_notice_show_timed(const char *message, uint32_t duration_ms) {
    if (message == NULL || message[0] == '\0' || lv_scr_act() == NULL) {
        /* No display yet: there is nothing to draw on and nobody to read it. */
        return;
    }
    if (notice_box == NULL || !lv_obj_is_valid(notice_box)) {
        notice_box = lv_obj_create(lv_layer_sys());
        lv_obj_set_size(notice_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(notice_box, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
        lv_obj_set_style_radius(notice_box, LV_DPX(5), 0);
        lv_obj_set_style_pad_hor(notice_box, LV_DPX(5), 0);
        lv_obj_set_style_pad_ver(notice_box, LV_DPX(3), 0);
        lv_obj_set_style_border_opa(notice_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(notice_box, LV_OPA_40, 0);
        lv_obj_set_style_bg_color(notice_box, lv_color_black(), 0);

        notice_label = lv_label_create(notice_box);
        lv_obj_set_size(notice_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(notice_label, lv_theme_get_font_small(notice_box), 0);
    }

    lv_label_set_text(notice_label, message);
    lv_obj_clear_flag(notice_box, LV_OBJ_FLAG_HIDDEN);

    if (notice_timer != NULL) {
        lv_timer_del(notice_timer);
    }
    notice_timer = lv_timer_create(notice_hide_cb, duration_ms, NULL);
    lv_timer_set_repeat_count(notice_timer, 1);
}

static void notice_hide_cb(lv_timer_t *timer) {
    /* repeat_count is 1, so lv_timer_handler deletes this timer once we return. */
    LV_UNUSED(timer);
    notice_timer = NULL;
    if (notice_box != NULL && lv_obj_is_valid(notice_box)) {
        lv_obj_add_flag(notice_box, LV_OBJ_FLAG_HIDDEN);
    }
}
