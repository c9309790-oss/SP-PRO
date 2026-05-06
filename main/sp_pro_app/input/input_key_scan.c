#include "sp_pro_app_types.h"
#include "sp_pro_app.h"
#include "sp_pro_app_state.h"
#include "esp_log.h"
#include <stddef.h>

typedef struct {
    uint8_t key_a;
    uint8_t key_b;
    key_combo_id_t combo_id;
} key_combo_rule_t;

static const key_combo_rule_t s_key_combo_rules[] = {
    { KEY_GRIND,     KEY_TEMP,   KEY_COMBO_WATER_IN_MODE },
    { KEY_CLEAN,     KEY_WIFI,   KEY_COMBO_CLEAR_PIPE },
    { KEY_CHILD,     KEY_CLEAN,  KEY_COMBO_FACTORY_RESET },
    { KEY_TEMP,      KEY_CHILD,  KEY_COMBO_WATER_HARDNESS },
    { KEY_ESPRESSO,  KEY_STEAM,  KEY_COMBO_CAL_POWDER },
    { KEY_AMERICANO, KEY_STEAM,  KEY_COMBO_CAL_FLOW },
    { KEY_COLD_BREW, KEY_STEAM,  KEY_COMBO_DETECTION },
#ifdef MAINT_TEST
    { KEY_ESPRESSO,  KEY_CLEAN,  KEY_COMBO_MAINT_BREW },
    { KEY_AMERICANO, KEY_CLEAN,  KEY_COMBO_MAINT_DES },
    { KEY_COLD_BREW, KEY_CLEAN,  KEY_COMBO_MAINT_STEAM },
#endif
};

static bool key_should_play_touch_tone(const app_ctx_t *ctx, int key_index)
{
    if (!ctx) {
        return false;
    }

    /* Avoid extra audio allocations while Wi-Fi pairing is being exited. */
    if (ctx->core.state == ST_WIFI && key_index == KEY_WIFI) {
        return false;
    }

    return true;
}

static uint16_t key_combo_rule_mask(const key_combo_rule_t *rule)
{
    if (!rule) {
        return 0U;
    }

    return (uint16_t)((1U << rule->key_a) | (1U << rule->key_b));
}

static bool key_combo_rule_is_down(const key_combo_rule_t *rule, uint16_t down_mask)
{
    uint16_t combo_mask = key_combo_rule_mask(rule);

    return combo_mask != 0U && (down_mask & combo_mask) == combo_mask;
}

static bool key_combo_rule_is_ready(const app_ctx_t *ctx, const key_combo_rule_t *rule)
{
    if (!ctx || !rule) {
        return false;
    }

    return ctx->input.key_time[rule->key_a] >= KEY_LONG_TICKS &&
           ctx->input.key_time[rule->key_b] >= KEY_LONG_TICKS;
}

static bool key_combo_rule_is_release_ready(const app_ctx_t *ctx, const key_combo_rule_t *rule)
{
    if (!ctx || !rule) {
        return false;
    }

    return ctx->input.key_time[rule->key_a] + 1U >= KEY_LONG_TICKS &&
           ctx->input.key_time[rule->key_b] + 1U >= KEY_LONG_TICKS;
}

static bool key_combo_try_fire(app_ctx_t *ctx, const key_combo_rule_t *rule, const char *reason)
{
    uint16_t combo_mask;

    if (!ctx || !rule || ctx->input.combo_lock_mask != 0U) {
        return false;
    }

    combo_mask = key_combo_rule_mask(rule);
    if (combo_mask == 0U) {
        return false;
    }

    ctx->input.key_combo = rule->combo_id;
    ctx->input.combo_lock_mask = 1U;
    ctx->input.key_long &= (uint16_t)~combo_mask;
    ctx->input.key_pressed &= (uint16_t)~combo_mask;
    ctx->input.long_lock_mask |= combo_mask;

    ESP_LOGI("key_scan",
             "Combo detected id=%u reason=%s down=0x%04X time_a=%u time_b=%u",
             (unsigned)rule->combo_id,
             reason ? reason : "unknown",
             (unsigned)ctx->input.key_down,
             (unsigned)ctx->input.key_time[rule->key_a],
             (unsigned)ctx->input.key_time[rule->key_b]);
    return true;
}

static bool key_has_combo_partner_down(const app_ctx_t *ctx, uint8_t key_index)
{
    if (!ctx || key_index >= KEY_COUNT || ctx->input.combo_lock_mask != 0U) {
        return false;
    }

    for (size_t i = 0; i < sizeof(s_key_combo_rules) / sizeof(s_key_combo_rules[0]); i++) {
        const key_combo_rule_t *rule = &s_key_combo_rules[i];
        uint8_t partner;

        if (rule->key_a == key_index) {
            partner = rule->key_b;
        } else if (rule->key_b == key_index) {
            partner = rule->key_a;
        } else {
            continue;
        }

        if ((ctx->input.key_down & (1U << partner)) != 0U) {
            return true;
        }
    }

    return false;
}

static void key_try_fire_release_combo(app_ctx_t *ctx, uint16_t old_mask)
{
    if (!ctx || ctx->input.combo_lock_mask != 0U) {
        return;
    }

    for (size_t i = 0; i < sizeof(s_key_combo_rules) / sizeof(s_key_combo_rules[0]); i++) {
        const key_combo_rule_t *rule = &s_key_combo_rules[i];

        if (!key_combo_rule_is_down(rule, old_mask)) {
            continue;
        }

        if (!key_combo_rule_is_release_ready(ctx, rule)) {
            continue;
        }

        if (key_combo_try_fire(ctx, rule, "release")) {
            return;
        }
    }
}

void key_scan_ticks(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (int i = 0; i < KEY_COUNT; i++) {
        if ((ctx->input.key_down & (1U << i)) == 0U) {
            continue;
        }

        if (ctx->input.key_time[i] < 0xFFFF) {
            ctx->input.key_time[i]++;
        }

        if (ctx->input.key_time[i] < KEY_LONG_TICKS ||
            (ctx->input.long_lock_mask & (1U << i)) != 0U) {
            continue;
        }

        /* If a combo partner is still held, defer single-key long-press dispatch
         * until combo arbitration finishes. This avoids entering a single-key
         * page one tick before the intended combo becomes eligible. */
        if (key_has_combo_partner_down(ctx, (uint8_t)i)) {
            continue;
        }

        if (key_should_play_touch_tone(ctx, i)) {
            voice_manager_play_touch_tone();
        }
        ctx->input.key_long |= (1U << i);
        ctx->input.long_lock_mask |= (1U << i);
    }
}

void key_combo_tick(app_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (size_t i = 0; i < sizeof(s_key_combo_rules) / sizeof(s_key_combo_rules[0]); i++) {
        const key_combo_rule_t *rule = &s_key_combo_rules[i];

        if (!key_combo_rule_is_down(rule, ctx->input.key_down)) {
            continue;
        }

        if (!key_combo_rule_is_ready(ctx, rule)) {
            continue;
        }

        if (key_combo_try_fire(ctx, rule, "hold")) {
            break;
        }
    }

    /* Allow a new combo after all keys are released. */
    if (ctx->input.key_down == 0U) {
        ctx->input.combo_lock_mask = 0U;
    }
}

void key_event_handle(app_ctx_t *ctx, const bf7613_key_event_t *event)
{
    uint16_t new_mask;
    uint16_t old_mask;

    if (!ctx || !event) {
        return;
    }

    new_mask = event->key_mask;
    old_mask = ctx->input.key_down;

    /* If both keys already satisfied the combo hold condition, latch the combo
     * before release ordering tears the chord apart. */
    key_try_fire_release_combo(ctx, old_mask);

    for (int i = 0; i < KEY_COUNT; i++) {
        bool now = (new_mask & (1U << i)) != 0U;
        bool last = (old_mask & (1U << i)) != 0U;

        /* Reset hold time on key press edge. */
        if (now && !last) {
            if (key_should_play_touch_tone(ctx, i)) {
                voice_manager_play_touch_tone();
            }
            ctx->input.key_time[i] = 0U;
            ctx->input.long_lock_mask &= (uint16_t)~(1U << i);
        }

        /* Convert release edge into a short press only when long press has not fired. */
        if (!now && last) {
            uint16_t held_ticks = ctx->input.key_time[i];

            if (held_ticks < KEY_LONG_TICKS) {
                ctx->input.key_pressed |= (1U << i);
            }

            ctx->input.key_time[i] = 0U;
            ctx->input.long_lock_mask &= (uint16_t)~(1U << i);
        }
    }

    ctx->input.key_down = new_mask;
}
