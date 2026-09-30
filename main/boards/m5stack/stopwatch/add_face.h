#pragma once

#include "crest_asset.h"
#include "crest_motion.h"
#include "lvgl.h"

LV_FONT_DECLARE(font_noto_sans_basic_16_4);

namespace ProvisionsStopWatch {

// Empty List/Notes is the resting face with a small action cue. Reuse the
// existing crest objects so pressing Talk can morph them into listening.
inline void PaintAddFace(lv_obj_t* band, lv_obj_t* star, lv_obj_t* title,
                         lv_obj_t* hint, const char* destination) {
    lv_obj_set_pos(band, OrbitCrest::kBandX, OrbitCrest::kBandY);
    lv_obj_set_pos(star, OrbitCrest::kStarX, OrbitCrest::kStarY);
    for (auto* mark : {band, star}) {
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_image_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_image_recolor(mark, lv_color_hex(OrbitCrest::kIvory), 0);
        lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
    }
    for (auto* label : {title, hint}) {
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(label, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_letter_space(label, 0, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(OrbitCrest::kIvory), 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    }
    lv_label_set_text(title, destination);
    lv_obj_set_size(title, 180, 24);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 66);
    lv_label_set_text(hint, "Hold yellow to speak");
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    lv_obj_set_size(hint, 210, 24);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 90);
}

}  // namespace ProvisionsStopWatch
