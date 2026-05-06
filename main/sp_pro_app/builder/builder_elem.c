#include "sp_pro_app_types.h"
#include "sp_pro_app_state.h"
#include <stdio.h>
#include <string.h>

static void clear_weight_left(disp_element_t *d, pos_indicator_t pos);
static void clear_weight_right(disp_element_t *d, pos_indicator_t pos);
static void clear_temp_left(disp_element_t *d, pos_indicator_t pos);
static void clear_temp_right(disp_element_t *d, pos_indicator_t pos);
static void disp_set_a_dual_dot_mode(disp_element_t *d, seg_position_t pos, bool enable);
static void disp_show_large_weight_value(disp_element_t *d, pos_indicator_t pos, float value);

/* ===== Parameter descriptors ===== */

static const disp_param_desc_t g_param_desc[] = {
    {
        .mode         = ENCODER_MODE_ESP_BREW_WEIGHT,
        .kind         = DISP_KIND_WEIGHT,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S2,
        .dot_id       = DISP_DOT_LEFT_WEIGHT,
        .gauge_enable = true,
        .gauge_max    = Q1_GAUGE_MAX_VAL,
        .clear_fn     = clear_weight_left,
    },
    {
        .mode         = ENCODER_MODE_ESP_BREW_TEMP,
        .kind         = DISP_KIND_TEMP,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S3 | BF_UNIT_S5,
        .clear_fn     = clear_temp_left,
    },
    {
        .mode         = ENCODER_MODE_AME_BREW_WEIGHT,
        .kind         = DISP_KIND_WEIGHT,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S2,
        .dot_id       = DISP_DOT_LEFT_WEIGHT,
        .gauge_enable = true,
        .gauge_max    = Q1_GAUGE_MAX_VAL,
        .clear_fn     = clear_weight_left,
    },
    {
        .mode         = ENCODER_MODE_AME_WATER_WEIGHT,
        .kind         = DISP_KIND_WEIGHT,
        .pos          = BF_POS_RIGHT,
        .unit_mask    = BF_UNIT_S8,
        .dot_id       = DISP_DOT_RIGHT_WEIGHT,
        .gauge_enable = true,
        .gauge_max    = Q2_GAUGE_MAX_VAL,
        .clear_fn     = clear_weight_right,
    },
    {
        .mode         = ENCODER_MODE_AME_BREW_TEMP,
        .kind         = DISP_KIND_TEMP,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S3 | BF_UNIT_S5,
        .clear_fn     = clear_temp_left,
    },
    {
        .mode         = ENCODER_MODE_AME_WATER_TEMP,
        .kind         = DISP_KIND_TEMP,
        .pos          = BF_POS_RIGHT,
        .unit_mask    = BF_UNIT_S9 | BF_UNIT_S10,
        .clear_fn     = clear_temp_right,
    },
    {
        .mode         = ENCODER_MODE_COLD_BREW_WEIGHT,
        .kind         = DISP_KIND_WEIGHT,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S2,
        .dot_id       = DISP_DOT_LEFT_WEIGHT,
        .gauge_enable = true,
        .gauge_max    = Q1_GAUGE_MAX_VAL,
        .clear_fn     = clear_weight_left,
    },
    {
        .mode         = ENCODER_MODE_HOT_WATER_WEIGHT,
        .kind         = DISP_KIND_WEIGHT,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S2,
        .dot_id       = DISP_DOT_LEFT_WEIGHT,
        .gauge_enable = true,
        .gauge_max    = Q1_GAUGE_MAX_VAL,
        .clear_fn     = clear_weight_left,
    },
    {
        .mode         = ENCODER_MODE_HOT_WATER_TEMP,
        .kind         = DISP_KIND_TEMP,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S4 | BF_UNIT_S5,
        .clear_fn     = clear_temp_left,
    },
    {
        .mode         = ENCODER_MODE_GRIND_WEIGHT,
        .kind         = DISP_KIND_WEIGHT,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S2,
        .dot_id       = DISP_DOT_LEFT_WEIGHT,
        .gauge_enable = true,
        .gauge_max    = Q1_GAUGE_MAX_VAL,
        .clear_fn     = clear_weight_left,
    },
    {
        .mode         = ENCODER_MODE_CLEAN_VOLUME,
        .kind         = DISP_KIND_WEIGHT,
        .pos          = BF_POS_LEFT,
        .unit_mask    = BF_UNIT_S2,
        .dot_id       = DISP_DOT_LEFT_WEIGHT,
        .gauge_enable = true,
        .gauge_max    = Q1_GAUGE_MAX_VAL,
        .clear_fn     = clear_weight_left,
    },
    {
        .mode         = ENCODER_MODE_STEAM_LEVEL,
        .kind         = DISP_KIND_LEVEL,
        .pos          = SM_POS_2,
        .digits       = 1,
        .clear_fn     = NULL,
    },
};

const disp_param_desc_t *disp_param_of(encoder_mode_t mode)
{
    for (int i = 0; i < (int)(sizeof(g_param_desc) / sizeof(g_param_desc[0])); i++) {
        if (g_param_desc[i].mode == mode) {
            return &g_param_desc[i];
        }
    }
    return NULL;
}

float disp_get_setting_value(const app_display_view_t *view, encoder_mode_t mode)
{
    if (!view) {
        return 0.0f;
    }

    switch (mode) {
    case ENCODER_MODE_ESP_BREW_WEIGHT:
        return view->setting.esp_brew_w;
    case ENCODER_MODE_ESP_BREW_TEMP:
        return view->setting.esp_brew_t;
    case ENCODER_MODE_AME_BREW_WEIGHT:
        return view->setting.ame_brew_w;
    case ENCODER_MODE_AME_WATER_WEIGHT:
        return view->setting.ame_water_w;
    case ENCODER_MODE_AME_BREW_TEMP:
        return view->setting.ame_brew_t;
    case ENCODER_MODE_AME_WATER_TEMP:
        return view->setting.ame_water_t;
    case ENCODER_MODE_COLD_BREW_WEIGHT:
        return view->setting.cold_brew_w;
    case ENCODER_MODE_HOT_WATER_WEIGHT:
        return view->setting.hot_water_w;
    case ENCODER_MODE_HOT_WATER_TEMP:
        return view->setting.hot_water_t;
    case ENCODER_MODE_GRIND_WEIGHT:
        return view->setting.grind_w;
    case ENCODER_MODE_CLEAN_VOLUME:
        return view->setting.clean_v;
    case ENCODER_MODE_STEAM_LEVEL:
        return view->setting.steam_level;
    case ENCODER_MODE_IDLE:
    case ENCODER_MODE_MAX:
    default:
        return 0.0f;
    }
}

setting_ui_phase_t ui_render_state(const app_display_view_t *view)
{
    if (!view || !view->setting.active) {
        return SET_UI_PHASE_STATIC;
    }

    return view->setting.ui_phase;
}

static inline bool ui_blink_on(const app_display_view_t *view)
{
    if (!view) {
        return false;
    }

    /* The UI blink period is 500 ms. */
    return ((view->tick / 25U) & 1U) != 0U;
}

bool render_visible(const app_display_view_t *view)
{
    return (ui_render_state(view) == SET_UI_PHASE_HINT) ? ui_blink_on(view) : true;
}

/* ================= Base helpers ================= */
void disp_clear(disp_element_t *d)
{
    if (!d) {
        return;
    }
    memset(d, 0, sizeof(disp_element_t));
}

/* ================= Segment helpers ================= */
static inline void disp_set_dot(disp_element_t *d, seg_dot_t dot, bool on)
{
    if (!d || dot >= DISP_DOT_COUNT) {
        return;
    }
    d->dots.dot[dot] = on;
}

static void disp_set_a_dual_dot_mode(disp_element_t *d, seg_position_t pos, bool enable)
{
    if (!d) {
        return;
    }

#if BF7613_PROTOCOL_HAS_BYTE29_SEGMENT_MODE
    switch (pos) {
    case SM_POS_1:
        d->segment_mode.sm1_a_dual_dot = enable;
        break;
    case SM_POS_2:
        d->segment_mode.sm2_a_dual_dot = enable;
        break;
    case SM_POS_3:
        d->segment_mode.sm3_a_dual_dot = enable;
        break;
    case SM_POS_6:
        d->segment_mode.sm6_a_dual_dot = enable;
        break;
    case SM_POS_7:
        d->segment_mode.sm7_a_dual_dot = enable;
        break;
    case SM_POS_8:
        d->segment_mode.sm8_a_dual_dot = enable;
        break;
    default:
        break;
    }
#else
    (void)pos;
    (void)enable;
#endif
}

void disp_show_char(disp_element_t *d, seg_position_t pos, seg7_char_t ch)
{
    if (!d || pos >= SM_POS_COUNT) {
        return;
    }

    /* V4 协议下，SM1/2/3/6/7/8 可用 Byt29 控制 A 段由 3 点改为左侧 2 点。
     * 当前先用于数字 1 的新字形。 */
    disp_set_a_dual_dot_mode(d, pos, ch == SEG_CHAR_1);
    d->segment.digit[pos] = bf7613_get_seg7_char_pos(pos, ch);
}

void disp_clear_char(disp_element_t *d, seg_position_t pos)
{
    if (!d || pos >= SM_POS_COUNT) {
        return;
    }

    disp_set_a_dual_dot_mode(d, pos, false);
    d->segment.digit[pos] = 0x00;
}

/* Display a zero-padded integer value. */
void disp_show_number(disp_element_t *d, seg_position_t start_pos, uint16_t number, uint8_t digits)
{
    if (!d || start_pos + digits > SM_POS_COUNT) {
        return;
    }

    char buffer[6];
    char format[8];

    snprintf(format, sizeof(format), "%%0%du", digits);
    snprintf(buffer, sizeof(buffer), format, number);

    size_t buffer_len = strlen(buffer);

    for (int i = 0; i < digits && i < (int)buffer_len; i++) {
        if (buffer[i] >= '0' && buffer[i] <= '9') {
            disp_show_char(d, start_pos + i, buffer[i] - '0');
        }
    }
}

/* ================= Key indicators ================= */
void disp_set_key_icon(disp_element_t *d, key_id_t key, white_light_level_t level)
{
    if (!d) {
        return;
    }

    switch (key) {
    case BF_KEY_K1:
        d->keys.byte14.k1_icon = level;
        break;
    case BF_KEY_K2:
        d->keys.byte14.k2_icon = level;
        break;
    case BF_KEY_K3:
        d->keys.byte15.k3_icon = level;
        break;
    case BF_KEY_K4:
        d->keys.byte15.k4_icon = level;
        break;
    case BF_KEY_K5:
        d->keys.byte16.k5_icon = level;
        break;
    case BF_KEY_K6:
        d->keys.byte16.k6_icon = level;
        break;
    case BF_KEY_K7:
        d->keys.byte16.k7_icon = level;
        break;
    case BF_KEY_K8:
        d->keys.byte17.k8_icon = level;
        break;
    case BF_KEY_K9:
        d->keys.byte17.k9_icon = level;
        break;
    case BF_KEY_K10:
        d->keys.byte17.k10_white = level;
        break;
    default:
        break;
    }
}

/* ================= Key labels ================= */
void disp_set_key_text(disp_element_t *d, key_id_t key, white_light_level_t level)
{
    if (!d) {
        return;
    }

    switch (key) {
    case BF_KEY_K1:
        d->keys.byte14.k1_text = level;
        break;
    case BF_KEY_K2:
        d->keys.byte14.k2_text = level;
        break;
    case BF_KEY_K3:
        d->keys.byte15.k3_text = level;
        break;
    case BF_KEY_K4:
        d->keys.byte15.k4_text = level;
        break;
    case BF_KEY_K5:
        d->keys.byte16.k5_text = level;
        break;
    default:
        break;
    }
}

/* ================= Key blink state ================= */
void disp_set_key_blink(disp_element_t *d, key_id_t key, bool blink)
{
    if (!d) {
        return;
    }

    switch (key) {
    case BF_KEY_K1:
        d->blink.byte23.k1_blink = blink;
        break;
    case BF_KEY_K2:
        d->blink.byte23.k2_blink = blink;
        break;
    case BF_KEY_K3:
        d->blink.byte23.k3_blink = blink;
        break;
    case BF_KEY_K4:
        d->blink.byte23.k4_blink = blink;
        break;
    case BF_KEY_K5:
        d->blink.byte23.k5_blink = blink;
        break;
    case BF_KEY_K6:
        d->blink.byte23.k6_blink = blink;
        break;
    case BF_KEY_K7:
        d->blink.byte23.k7_blink = blink;
        break;
    case BF_KEY_K8:
        d->blink.byte23.k8_blink = blink;
        break;
    case BF_KEY_K9:
        d->blink.byte24.k9_blink = blink;
        break;
    case BF_KEY_K10:
        d->blink.byte24.k10_blink = blink;
        break;
    default:
        break;
    }
}

/* ================= L indicators ================= */
void disp_set_l_indicator(disp_element_t *d, l_indicator_t indicator, uint8_t level)
{
    if (!d) {
        return;
    }

    switch (indicator) {
    case BF_INDICATOR_L1:
        d->icons.byte20.l1 = level;
        break;
    case BF_INDICATOR_L2:
        d->icons.byte20.l2 = level;
        break;
    case BF_INDICATOR_L3:
        d->icons.byte20.l3 = level;
        break;
    case BF_INDICATOR_L4:
        d->icons.byte20.l4 = level;
        break;
    default:
        break;
    }
}

/* ================= Unit indicators ================= */
void disp_set_s_indicator(disp_element_t *d, unit_indicator_t unit, bool on, bool blink)
{
    if (!d) {
        return;
    }

    switch (unit) {
    case BF_UNIT_S1:  // mL
        d->icons.byte18.s1 = on;
        d->blink.byte25.s1_blink = blink;
        break;
    case BF_UNIT_S2:  // g
        d->icons.byte18.s2 = on;
        d->blink.byte25.s2_blink = blink;
        break;
    case BF_UNIT_S3:  // coffee outlet icon
        d->icons.byte18.s3 = on;
        d->blink.byte25.s3_blink = blink;
        break;
    case BF_UNIT_S4:  // hot water outlet icon
        d->icons.byte19.s4 = on;
        d->blink.byte25.s4_blink = blink;
        break;
    case BF_UNIT_S5:  // Celsius symbol
        d->icons.byte19.s5 = on;
        d->blink.byte25.s5_blink = blink;
        break;
    case BF_UNIT_S6:  // mL
        d->icons.byte19.s6 = on;
        d->blink.byte25.s6_blink = blink;
        break;
    case BF_UNIT_S7:  // bar
        d->icons.byte19.s7 = on;
        d->blink.byte25.s7_blink = blink;
        break;
    case BF_UNIT_S8:  // g
        d->icons.byte19.s8 = on;
        d->blink.byte25.s8_blink = blink;
        break;
    case BF_UNIT_S9:  // degree symbol
        d->icons.byte19.s9 = on;
        d->blink.byte26.s9_blink = blink;
        break;
    case BF_UNIT_S10:  // Celsius symbol
        d->icons.byte19.s10 = on;
        d->blink.byte26.s10_blink = blink;
        break;
    case BF_UNIT_S11: // seconds
        d->icons.byte19.s11 = on;
        d->blink.byte26.s11_blink = blink;
        break;
    default:
        break;
    }
}

/* ================= Status indicators ================= */
void disp_set_p_indicator(disp_element_t *d, p_indicator_t status, bool on, bool blink)
{
    if (!d) {
        return;
    }

    switch (status) {
    case BF_STATUS_P1:  // fault indicator
        d->icons.byte18.p1 = on;
        d->blink.byte24.p1_red_blink = blink;
        break;
    case BF_STATUS_P2:  // warning indicator
        d->icons.byte18.p2 = on;
        d->blink.byte24.p2_red_blink = blink;
        break;
    case BF_STATUS_P3:  // maintenance indicator
        d->icons.byte18.p3 = on;
        d->blink.byte24.p3_red_blink = blink;
        break;
    case BF_STATUS_P4:  // consumable indicator
        d->icons.byte18.p4 = on;
        d->blink.byte24.p4_red_blink = blink;
        break;
    case BF_STATUS_P5:  // lock indicator
        d->icons.byte18.p5 = on;
        d->blink.byte24.p5_red_blink = blink;
        break;
    default:
        break;
    }
}

/* Clear every status indicator and its blink flag. */
void disp_clear_all_p_indicators(disp_element_t *d)
{
    if (!d) {
        return;
    }

    d->icons.byte18.p1 = false;
    d->icons.byte18.p2 = false;
    d->icons.byte18.p3 = false;
    d->icons.byte18.p4 = false;
    d->icons.byte18.p5 = false;

    d->blink.byte24.p1_red_blink = false;
    d->blink.byte24.p2_red_blink = false;
    d->blink.byte24.p3_red_blink = false;
    d->blink.byte24.p4_red_blink = false;
    d->blink.byte24.p5_red_blink = false;
}

/* ===== Value clear helpers ===== */

static void clear_weight_left(disp_element_t *d, pos_indicator_t pos)
{
    (void)pos;
    disp_clear_char(d, SM_POS_1);
    disp_clear_char(d, SM_POS_2);
    disp_clear_char(d, SM_POS_3);
    disp_set_dot(d, DISP_DOT_LEFT_WEIGHT, false);
}

static void clear_weight_right(disp_element_t *d, pos_indicator_t pos)
{
    (void)pos;
    disp_clear_char(d, SM_POS_6);
    disp_clear_char(d, SM_POS_7);
    disp_clear_char(d, SM_POS_8);
    disp_set_dot(d, DISP_DOT_RIGHT_WEIGHT, false);
}

static void clear_temp_left(disp_element_t *d, pos_indicator_t pos)
{
    (void)pos;
    disp_clear_char(d, SM_POS_4);
    disp_clear_char(d, SM_POS_5);
}

static void clear_temp_right(disp_element_t *d, pos_indicator_t pos)
{
    (void)pos;
    disp_clear_char(d, SM_POS_9);
    disp_clear_char(d, SM_POS_10);
}

static void disp_show_large_weight_value(disp_element_t *d, pos_indicator_t pos, float value)
{
    seg_position_t pos_1;
    seg_position_t pos_2;
    seg_position_t pos_3;
    uint16_t value_x10;

    if (!d) {
        return;
    }

    if (pos == BF_POS_LEFT) {
        pos_1 = SM_POS_1;
        pos_2 = SM_POS_2;
        pos_3 = SM_POS_3;
    } else if (pos == BF_POS_RIGHT) {
        pos_1 = SM_POS_6;
        pos_2 = SM_POS_7;
        pos_3 = SM_POS_8;
    } else {
        return;
    }

    value_x10 = (uint16_t)(value * 10.0f + 0.5f);

    /* Large 3-digit area can only show:
     * - 0.0 ~ 99.9 with one decimal
     * - 100 ~ 999 as integer
     * Once value reaches 100.0, switch to integer rendering immediately,
     * otherwise the hundreds digit would become 10/11/... and map to
     * unexpected segment glyphs such as 'H'. */
    if (value_x10 >= 1000U || (value_x10 % 10U) == 0U) {
        uint16_t integer_value = (uint16_t)(value + 0.5f);
        if (integer_value > 999U) {
            integer_value = 999U;
        }

        if (integer_value >= 100U) {
            disp_show_char(d, pos_1, (integer_value / 100U) % 10U);
        } else {
            disp_clear_char(d, pos_1);
        }

        if (integer_value >= 10U) {
            disp_show_char(d, pos_2, (integer_value / 10U) % 10U);
        } else if (integer_value >= 100U) {
            disp_show_char(d, pos_2, (integer_value / 10U) % 10U);
        } else {
            disp_clear_char(d, pos_2);
        }

        disp_show_char(d, pos_3, integer_value % 10U);
        disp_set_dot(d, pos == BF_POS_LEFT ? DISP_DOT_LEFT_WEIGHT : DISP_DOT_RIGHT_WEIGHT, false);
        return;
    }

    disp_show_char(d, pos_1, (value_x10 / 10U) / 10U);
    disp_show_char(d, pos_2, (value_x10 / 10U) % 10U);
    disp_show_char(d, pos_3, value_x10 % 10U);
    disp_set_dot(d, pos == BF_POS_LEFT ? DISP_DOT_LEFT_WEIGHT : DISP_DOT_RIGHT_WEIGHT, true);
}

/* Display an H-code fault on the seven-segment area. */
void disp_show_h_code(disp_element_t *d, uint8_t major, uint8_t sub)
{
    if (!d) {
        return;
    }

    /* Clear all segment digits first. */
    for (int i = 0; i < SM_POS_COUNT; i++) {
        d->segment.digit[i] = bf7613_get_seg7_char_pos(i, SEG_CHAR_BLANK);
    }

    disp_show_char(d, SM_POS_1, SEG_CHAR_H);

    /* Render the main fault code. */
    disp_show_char(d, SM_POS_2, major / 10);
    disp_show_char(d, SM_POS_3, major % 10);

    /* Render the optional sub-code. */
    if (sub > 0) {
        disp_show_char(d, SM_POS_4, sub);
    }

    /* Turn on all red status markers for a hard fault page. */
    disp_set_p_indicator(d, BF_STATUS_P1, true, false);
    disp_set_p_indicator(d, BF_STATUS_P2, true, false);
    disp_set_p_indicator(d, BF_STATUS_P3, true, false);
    disp_set_p_indicator(d, BF_STATUS_P4, true, false);
    disp_set_p_indicator(d, BF_STATUS_P5, true, false);
}

/* ================= Gauge helpers ================= */
void disp_set_gauge_q1(disp_element_t *d, uint8_t level)
{
    if (!d || level > Q1_GAUGE_MAX_VAL) {
        return;
    }
    d->q1_gauge = level;
}

void disp_set_gauge_q2(disp_element_t *d, uint8_t level)
{
    if (!d || level > Q2_GAUGE_MAX_VAL) {
        return;
    }
    d->q2_gauge = level;
}

void disp_set_gauge_percent(disp_element_t *d, uint8_t pos, uint8_t total, uint8_t percent)
{
    if (!d) {
        return;
    }

    uint8_t level = (percent * total) / 100U;
    if (level > total) {
        level = total;
    }

    if (pos == BF_POS_LEFT) {
        disp_set_gauge_q1(d, level);
    } else if (pos == BF_POS_RIGHT) {
        disp_set_gauge_q2(d, level);
    }
}

/* ================= Numeric display helpers ================= */
void disp_show_weight_with_unit(disp_element_t *d,
                                pos_indicator_t pos,
                                float weight,
                                float target,
                                bool use_gram_unit)
{
    if (!d) {
        return;
    }

    uint16_t value_x10 = (uint16_t)(weight * 10.0f + 0.5f);
    uint16_t target_x10 = (uint16_t)(target * 10.0f + 0.5f);

    if (pos == BF_POS_LEFT) {
        disp_show_large_weight_value(d, BF_POS_LEFT, weight);
        disp_set_s_indicator(d, BF_UNIT_S1, !use_gram_unit, false);
        disp_set_s_indicator(d, BF_UNIT_S2, use_gram_unit, false);
    } else if (pos == BF_POS_RIGHT) {
        disp_show_large_weight_value(d, BF_POS_RIGHT, weight);
        disp_set_s_indicator(d, BF_UNIT_S6, !use_gram_unit, false);
        disp_set_s_indicator(d, BF_UNIT_S8, use_gram_unit, false);
    }

    if (target_x10 > 0U) {
        uint8_t percent = (value_x10 * 100U) / target_x10;
        if (percent > 100U) {
            percent = 100U;
        }
        disp_set_gauge_percent(
            d,
            pos,
            pos == BF_POS_LEFT ? Q1_GAUGE_MAX_VAL : Q2_GAUGE_MAX_VAL,
            percent);
    }
}

void disp_show_weight(disp_element_t *d, pos_indicator_t pos, float weight_g, float target_g)
{
    disp_show_weight_with_unit(d, pos, weight_g, target_g, false);
}

void disp_show_cup_calibration(disp_element_t *d,
                               pos_indicator_t pos,
                               float coeff,
                               int8_t adjust_percent)
{
    uint8_t percent;
    uint8_t gauge_max;
    int display_value;

    if (!d) {
        return;
    }

    (void)coeff;

    /* Show the current adjustment around a neutral 100 baseline:
     * 100 = no extra adjustment, 101 = +1%, 99 = -1%. */
    display_value = 100 + (int)adjust_percent;
    if (display_value < 0) {
        display_value = 0;
    } else if (display_value > 999) {
        display_value = 999;
    }
    disp_show_large_weight_value(d, pos, (float)display_value);

    if (pos == BF_POS_LEFT) {
        disp_set_s_indicator(d, BF_UNIT_S1, false, false);
        disp_set_s_indicator(d, BF_UNIT_S2, false, false);
        gauge_max = Q1_GAUGE_MAX_VAL;
    } else if (pos == BF_POS_RIGHT) {
        disp_set_s_indicator(d, BF_UNIT_S6, false, false);
        disp_set_s_indicator(d, BF_UNIT_S8, false, false);
        gauge_max = Q2_GAUGE_MAX_VAL;
    } else {
        return;
    }

    if (adjust_percent < -20) {
        adjust_percent = -20;
    } else if (adjust_percent > 20) {
        adjust_percent = 20;
    }

    percent = (uint8_t)(((int)adjust_percent + 20) * 100 / 40);
    disp_set_gauge_percent(d, pos, gauge_max, percent);
}

void disp_show_brew_yield(disp_element_t *d,
                          pos_indicator_t pos,
                          float value,
                          float target,
                          bool use_gram_unit)
{
    if (!d) {
        return;
    }

    uint16_t shown_value = (uint16_t)(value + 0.5f);
    uint16_t shown_target = (uint16_t)(target + 0.5f);
    uint8_t hundreds = (shown_value / 100U) % 10U;
    uint8_t tens = (shown_value / 10U) % 10U;
    uint8_t ones = shown_value % 10U;

    if (pos == BF_POS_LEFT) {
        disp_show_char(d, SM_POS_1, hundreds);
        disp_show_char(d, SM_POS_2, tens);
        disp_show_char(d, SM_POS_3, ones);
        disp_set_dot(d, DISP_DOT_LEFT_WEIGHT, false);
        disp_set_s_indicator(d, BF_UNIT_S1, !use_gram_unit, false);
        disp_set_s_indicator(d, BF_UNIT_S2, use_gram_unit, false);
    } else if (pos == BF_POS_RIGHT) {
        disp_show_char(d, SM_POS_6, hundreds);
        disp_show_char(d, SM_POS_7, tens);
        disp_show_char(d, SM_POS_8, ones);
        disp_set_dot(d, DISP_DOT_RIGHT_WEIGHT, false);
        disp_set_s_indicator(d, BF_UNIT_S6, !use_gram_unit, false);
        disp_set_s_indicator(d, BF_UNIT_S8, use_gram_unit, false);
    }

    if (shown_target > 0U) {
        uint8_t percent = (shown_value * 100U) / shown_target;
        if (percent > 100U) {
            percent = 100U;
        }
        disp_set_gauge_percent(
            d,
            pos,
            pos == BF_POS_LEFT ? Q1_GAUGE_MAX_VAL : Q2_GAUGE_MAX_VAL,
            percent);
    }
}

void disp_show_grind_w(disp_element_t *d, float weight_g, float target_g)
{
    if (!d) {
        return;
    }

    uint16_t target_x10 = (uint16_t)(target_g * 10.0f + 0.5f);
    uint16_t value_x10 = (uint16_t)(weight_g * 10.0f + 0.5f);

    disp_show_large_weight_value(d, BF_POS_LEFT, weight_g);
    disp_set_s_indicator(d, BF_UNIT_S2, true, false);

    if (target_x10 > 0U) {
        uint8_t percent = (value_x10 * 100U) / target_x10;
        if (percent > 100U) {
            percent = 100U;
        }
        disp_set_gauge_percent(d, BF_POS_LEFT, Q1_GAUGE_MAX_VAL, percent);
    }
}

/* ================= Brew pressure helpers ================= */
void disp_show_pressure_bar(disp_element_t *d, float value_bar, float max_bar)
{
    if (!d) {
        return;
    }

    uint16_t value_x10 = (uint16_t)(value_bar * 10.0f + 0.5f);
    uint16_t max_x10 = (uint16_t)(max_bar * 10.0f + 0.5f);
    uint8_t tens = (value_x10 / 10U) / 10U;
    uint8_t ones = (value_x10 / 10U) % 10U;
    uint8_t tenths = value_x10 % 10U;

    disp_show_char(d, SM_POS_6, tens);
    disp_show_char(d, SM_POS_7, ones);
    disp_show_char(d, SM_POS_8, tenths);
    disp_set_dot(d, DISP_DOT_RIGHT_WEIGHT, true);
    disp_set_s_indicator(d, BF_UNIT_S7, true, false);

    if (max_x10 > 0U) {
        uint8_t percent = (value_x10 * 100U) / max_x10;
        if (percent > 100U) {
            percent = 100U;
        }
        disp_set_gauge_percent(d, BF_POS_RIGHT, Q2_GAUGE_MAX_VAL, percent);
    }
}

/* ================= Temperature helpers ================= */
void disp_show_temp(disp_element_t *d, pos_indicator_t pos, uint8_t temp)
{
    if (!d) {
        return;
    }

    switch (pos) {
    case BF_POS_LEFT:
        disp_show_number(d, SM_POS_4, temp, 2);
        disp_set_s_indicator(d, BF_UNIT_S3, true, false);
        disp_set_s_indicator(d, BF_UNIT_S5, true, false);
        break;
    case BF_POS_RIGHT:
        disp_show_number(d, SM_POS_9, temp, 2);
        disp_set_s_indicator(d, BF_UNIT_S9, true, false);
        disp_set_s_indicator(d, BF_UNIT_S10, true, false);
        break;
    default:
        break;
    }
}

/* ================= Time helpers ================= */
void disp_show_time_s(disp_element_t *d, uint16_t seconds)
{
    if (!d) {
        return;
    }

    disp_show_number(d, SM_POS_11, seconds, 2);
    disp_set_s_indicator(d, BF_UNIT_S11, true, false);
}

/* ================= Animation helpers ================= */
void disp_preheat_marquee(disp_element_t *d, uint8_t step)
{
    if (!d) {
        return;
    }

    uint8_t idx = step % 5U;

    for (uint8_t i = 0; i < 5U; i++) {
        disp_set_key_icon(d, i, WHITE_LIGHT_OFF);
        disp_set_key_text(d, i, WHITE_LIGHT_OFF);
    }

    disp_set_key_icon(d, idx, WHITE_LIGHT_FULL);
    disp_set_key_text(d, idx, WHITE_LIGHT_FULL);

    disp_set_p_indicator(d, BF_STATUS_P1, false, false);
    disp_set_p_indicator(d, BF_STATUS_P2, false, false);
    disp_set_p_indicator(d, BF_STATUS_P3, false, false);
    disp_set_p_indicator(d, BF_STATUS_P4, false, false);
    disp_set_p_indicator(d, BF_STATUS_P5, false, false);
}

/* ================= Audio helpers ================= */
void disp_set_voice_play(disp_element_t *d, uint8_t voice_seq, uint8_t voice_data)
{
    if (!d) {
        return;
    }
    d->voice_seq = voice_seq;
    d->voice_data = voice_data;
}

/* ---------- Key backlight policies ---------- */
void disp_apply_key_mode(const app_display_view_t *view,
                         disp_element_t *d,
                         key_display_mode_t mode,
                         uint16_t full_mask,
                         uint16_t blink_mask)
{
    if (!d) {
        return;
    }

    full_mask &= (uint16_t)~blink_mask;

    for (int i = 0; i < BF_KEY_COUNT; i++) {
        disp_set_key_icon(d, i, WHITE_LIGHT_HALF);
        disp_set_key_text(d, i, WHITE_LIGHT_HALF);
        disp_set_key_blink(d, i, false);
    }

    switch (mode) {
    case KEY_MODE_READY:
        for (int i = 0; i < BF_KEY_COUNT; i++) {
            if (i <= BF_KEY_K5) {
                disp_set_key_icon(d, i, WHITE_LIGHT_FULL);
                disp_set_key_text(d, i, WHITE_LIGHT_HALF);
            } else if (i == BF_KEY_K8) {
                bool child_lock_enabled = view && view->child_lock.enabled;
                disp_set_key_icon(
                    d,
                    i,
                    child_lock_enabled ? WHITE_LIGHT_FULL : WHITE_LIGHT_HALF);
                disp_set_key_text(
                    d,
                    i,
                    child_lock_enabled ? WHITE_LIGHT_FULL : WHITE_LIGHT_HALF);
            }
        }

        if (view && view->network_connected) {
            d->keys.byte17.k10_white = WHITE_LIGHT_OFF;
            d->keys.byte17.k10_blue = WHITE_LIGHT_FULL;
        }
        break;

    case KEY_MODE_SINGLE:
        if (full_mask) {
            int key = __builtin_ctz(full_mask);
            disp_set_key_icon(d, key, WHITE_LIGHT_FULL);
            disp_set_key_text(d, key, WHITE_LIGHT_FULL);
        }
        break;

    case KEY_MODE_SINGLE_BLINK:
        if (blink_mask) {
            int key = __builtin_ctz(blink_mask);
            disp_set_key_icon(d, key, WHITE_LIGHT_FULL);
            disp_set_key_text(d, key, WHITE_LIGHT_FULL);
            disp_set_key_blink(d, key, true);
        }
        break;

    case KEY_MODE_COMBO_BLINK:
        for (int i = 0; i < BF_KEY_COUNT; i++) {
            uint16_t bit = (uint16_t)(1U << i);

            if (blink_mask & bit) {
                disp_set_key_icon(d, i, WHITE_LIGHT_FULL);
                disp_set_key_text(d, i, WHITE_LIGHT_FULL);
                disp_set_key_blink(d, i, true);
            } else if (full_mask & bit) {
                disp_set_key_icon(d, i, WHITE_LIGHT_FULL);
                disp_set_key_text(d, i, WHITE_LIGHT_FULL);
            }
        }
        break;

    case KEY_MODE_CHILD_LOCK:
        disp_set_key_icon(d, BF_KEY_K8, WHITE_LIGHT_FULL);
        disp_set_key_text(d, BF_KEY_K8, WHITE_LIGHT_FULL);
        break;

    case KEY_MODE_ALL_OFF:
    default:
        break;
    }
}

