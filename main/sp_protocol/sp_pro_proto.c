#include "sp_pro_proto.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

#define TAG "sp_pro_proto"

/* ================= CRC16 计算 ================= */

static const uint16_t crc16_table[16] = {
    0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400
};


uint16_t bf7613_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        crc = crc16_table[(byte ^ crc) & 0x0F] ^ (crc >> 4);
        crc = crc16_table[((byte >> 4) ^ crc) & 0x0F] ^ (crc >> 4);
    }

    return crc;
}

// 7 段数码管字模表
// Large digits: SM1/SM2/SM3/SM6/SM7/SM8
// Small digits: SM4/SM5/SM9/SM10/SM11/SM12
// NOTE: in protocol V4, byte29 adds per-position A-segment dual-dot enable
// for SM1/2/3/6/7/8. In protocol V3 this byte does not exist.
static const uint8_t seg7_char_table_large[SEG_CHAR_COUNT] = {
    [SEG_CHAR_0] = 0x3F,
    [SEG_CHAR_1] = 0x89,
    [SEG_CHAR_2] = 0x5B,
    [SEG_CHAR_3] = 0x4F,
    [SEG_CHAR_4] = 0x66,
    [SEG_CHAR_5] = 0x6D,
    [SEG_CHAR_6] = 0x7D,
    [SEG_CHAR_7] = 0x07,
    [SEG_CHAR_8] = 0x7F,
    [SEG_CHAR_9] = 0x6F,
    [SEG_CHAR_H] = 0x76,
    [SEG_CHAR_BLANK] = 0x00,
};

static const uint8_t seg7_char_table_small[SEG_CHAR_COUNT] = {
    [SEG_CHAR_0] = 0x3F,
    [SEG_CHAR_1] = 0x06,
    [SEG_CHAR_2] = 0x5B,
    [SEG_CHAR_3] = 0x4F,
    [SEG_CHAR_4] = 0x66,
    [SEG_CHAR_5] = 0x6D,
    [SEG_CHAR_6] = 0x7D,
    [SEG_CHAR_7] = 0x07,
    [SEG_CHAR_8] = 0x7F,
    [SEG_CHAR_9] = 0x6F,
    [SEG_CHAR_H] = 0x76,
    [SEG_CHAR_BLANK] = 0x00,
};

static inline seg_pos_group_t seg_pos_group(seg_position_t pos)
{
    switch (pos) {
    case SM_POS_1:
    case SM_POS_2:
    case SM_POS_3:
    case SM_POS_6:
    case SM_POS_7:
    case SM_POS_8:
        return SEG_POS_GROUP_LARGE;
    default:
        return SEG_POS_GROUP_SMALL;
    }
}

uint8_t bf7613_get_seg7_char_pos(seg_position_t pos, seg7_char_t ch)
{
    if (ch >= SEG_CHAR_COUNT) {
        ch = SEG_CHAR_BLANK;
    }

    if (seg_pos_group(pos) == SEG_POS_GROUP_LARGE) {
        return seg7_char_table_large[ch];
    }

    return seg7_char_table_small[ch];
}

/* 打印当前按下的按键信息 */
static void print_key_details(uint16_t key_mask)
{
    static const char *key_names[BF7613_KEY_NAME_COUNT] = {
        "K1(ESPRESSO)", "K2(AMERICA)", "K3(COLD)", "K4(WATER)", "K5(STEAM)",
        "K6(GRIND)", "K7(TEMP)", "K8(LOCK)", "K9(CLEAN)", "K10(WiFi)"
    };

    char key_list[128] = "";
    size_t offset = 0;
    int count = 0;

    for (int i = 0; i < BF7613_KEY_NAME_COUNT; i++) {
        if ((key_mask & (1U << i)) == 0) {
            continue;
        }

        int written = snprintf(key_list + offset,
                               sizeof(key_list) - offset,
                               "%s%s",
                               count > 0 ? ", " : "",
                               key_names[i]);
        if (written < 0 || (size_t)written >= sizeof(key_list) - offset) {
            break;
        }

        offset += (size_t)written;
        count++;
    }

    if (count > 0) {
        ESP_LOGI(TAG, "Pressed keys: %s", key_list);
    }
}

/* ================= 按键帧解析 ================= */

bool bf7613_parse_key_frame(const uint8_t *buf, uint16_t len, bf7613_key_event_t *event)
{
    /* 参数和帧长度校验 */
    if (!buf || !event || len != BF7613_KEY_FRAME_LEN) {
        return false;
    }

    /* 帧头与固定字节校验 */
    if (buf[0] != 0xA5 || buf[1] != 0x55 || buf[5] != 0x55) {
        return false;
    }

    /* CRC 校验 */
    uint16_t crc_received = (buf[7] << 8) | buf[6];
    uint16_t crc_calculated = bf7613_crc16(buf, 6);
    if (crc_received != crc_calculated) {
        return false;
    }

    /* 清空按键状态 */
    event->key_mask = 0;

    /* Byt2: K1-K8 */
    for (int i = 0; i < 8; i++) {
        if (buf[2] & (1U << i)) {
            event->key_mask |= (1U << i);
        }
    }

    /* Byt3: K9-K10 */
    for (int i = 0; i < 2; i++) {
        if (buf[3] & (1U << i)) {
            event->key_mask |= (1U << (8 + i));
        }
    }

    event->frame_id = buf[4];
    print_key_details(event->key_mask);

    return true;
}

/* ================= 显示帧构建 ================= */

bool bf7613_build_display_frame(const disp_element_t *display,
                                uint8_t *frame_buf,
                                uint16_t buf_len)
{
    if (!display || !frame_buf || buf_len < BF7613_DISPLAY_FRAME_LEN) {
        return false;
    }

    memset(frame_buf, 0, BF7613_DISPLAY_FRAME_LEN);

    /* ---------- 帧头 ---------- */
    frame_buf[0] = 0xA5;
    frame_buf[1] = 0x55;

    /* ---------- 数码管段码(Byt2~Byt13) ---------- */
    for (int i = 0; i < SEG_CHAR_COUNT; i++) {
        frame_buf[2 + i] = display->segment.digit[i];
    }

    /*
     * bit7 用于小数点
     * - SM4.bit7 -> SM2 左侧重量点
     * - SM5.bit7 -> SM7 右侧重量点
     * 其余 SM 位的 bit7 = 保留位
     */
    frame_buf[2 + SM_POS_4] &= 0x7F;
    frame_buf[2 + SM_POS_5] &= 0x7F;

    if (display->dots.dot[DISP_DOT_LEFT_WEIGHT]) {
        frame_buf[2 + SM_POS_4] |= 0x80;
    }

    if (display->dots.dot[DISP_DOT_RIGHT_WEIGHT]) {
        frame_buf[2 + SM_POS_5] |= 0x80;
    }

    /* ---------- 按键灯状态 (Byt14~Byt17) ---------- */
    frame_buf[14] = (display->keys.byte14.k1_icon & 0x03) |
                    ((display->keys.byte14.k1_text & 0x03) << 2) |
                    ((display->keys.byte14.k2_icon & 0x03) << 4) |
                    ((display->keys.byte14.k2_text & 0x03) << 6);

    frame_buf[15] = (display->keys.byte15.k3_icon & 0x03) |
                    ((display->keys.byte15.k3_text & 0x03) << 2) |
                    ((display->keys.byte15.k4_icon & 0x03) << 4) |
                    ((display->keys.byte15.k4_text & 0x03) << 6);

    frame_buf[16] = (display->keys.byte16.k5_icon & 0x03) |
                    ((display->keys.byte16.k5_text & 0x03) << 2) |
                    ((display->keys.byte16.k6_icon & 0x03) << 4) |
                    ((display->keys.byte16.k7_icon & 0x03) << 6);

    frame_buf[17] = (display->keys.byte17.k8_icon & 0x03) |
                    ((display->keys.byte17.k9_icon & 0x03) << 2) |
                    ((display->keys.byte17.k10_white & 0x03) << 4) |
                    ((display->keys.byte17.k10_blue & 0x03) << 6);

    /* ---------- 图标 ---------- */
    frame_buf[18] = (display->icons.byte18.p1 ? 0x01 : 0) |
                    (display->icons.byte18.p2 ? 0x02 : 0) |
                    (display->icons.byte18.p3 ? 0x04 : 0) |
                    (display->icons.byte18.p4 ? 0x08 : 0) |
                    (display->icons.byte18.p5 ? 0x10 : 0) |
                    (display->icons.byte18.s1 ? 0x20 : 0) |
                    (display->icons.byte18.s2 ? 0x40 : 0) |
                    (display->icons.byte18.s3 ? 0x80 : 0);

    frame_buf[19] = (display->icons.byte19.s4 ? 0x01 : 0) |
                    (display->icons.byte19.s5 ? 0x02 : 0) |
                    (display->icons.byte19.s6 ? 0x04 : 0) |
                    (display->icons.byte19.s7 ? 0x08 : 0) |
                    (display->icons.byte19.s8 ? 0x10 : 0) |
                    (display->icons.byte19.s9 ? 0x20 : 0) |
                    (display->icons.byte19.s10 ? 0x40 : 0) |
                    (display->icons.byte19.s11 ? 0x80 : 0);

    frame_buf[20] = (display->icons.byte20.l1 & 0x03) |
                    ((display->icons.byte20.l2 & 0x03) << 2) |
                    ((display->icons.byte20.l3 & 0x03) << 4) |
                    ((display->icons.byte20.l4 & 0x03) << 6);

    /* ---------- 进度条 ---------- */
    frame_buf[21] = display->q1_gauge;
    frame_buf[22] = display->q2_gauge;

    /* ---------- 闪烁控制 (Byt23-Byt26) ---------- */
    frame_buf[23] = (display->blink.byte23.k1_blink ? 0x01 : 0) |
                    (display->blink.byte23.k2_blink ? 0x02 : 0) |
                    (display->blink.byte23.k3_blink ? 0x04 : 0) |
                    (display->blink.byte23.k4_blink ? 0x08 : 0) |
                    (display->blink.byte23.k5_blink ? 0x10 : 0) |
                    (display->blink.byte23.k6_blink ? 0x20 : 0) |
                    (display->blink.byte23.k7_blink ? 0x40 : 0) |
                    (display->blink.byte23.k8_blink ? 0x80 : 0);

    frame_buf[24] = (display->blink.byte24.k9_blink ? 0x01 : 0) |
                    (display->blink.byte24.k10_blink ? 0x02 : 0) |
                    (display->blink.byte24.p1_red_blink ? 0x04 : 0) |
                    (display->blink.byte24.p2_red_blink ? 0x08 : 0) |
                    (display->blink.byte24.p3_red_blink ? 0x10 : 0) |
                    (display->blink.byte24.p4_red_blink ? 0x20 : 0) |
                    (display->blink.byte24.p5_red_blink ? 0x40 : 0);

    frame_buf[25] = (display->blink.byte25.s1_blink ? 0x01 : 0) |
                    (display->blink.byte25.s2_blink ? 0x02 : 0) |
                    (display->blink.byte25.s3_blink ? 0x04 : 0) |
                    (display->blink.byte25.s4_blink ? 0x08 : 0) |
                    (display->blink.byte25.s5_blink ? 0x10 : 0) |
                    (display->blink.byte25.s6_blink ? 0x20 : 0) |
                    (display->blink.byte25.s7_blink ? 0x40 : 0) |
                    (display->blink.byte25.s8_blink ? 0x80 : 0);

    frame_buf[26] = (display->blink.byte26.s9_blink ? 0x01 : 0) |
                    (display->blink.byte26.s10_blink ? 0x02 : 0) |
                    (display->blink.byte26.s11_blink ? 0x04 : 0) |
                    (display->blink.byte26.l1_blink ? 0x08 : 0) |
                    (display->blink.byte26.l2_blink ? 0x10 : 0) |
                    (display->blink.byte26.l3_blink ? 0x20 : 0) |
                    (display->blink.byte26.l4_blink ? 0x40 : 0);

    /* ---------- 语音控制 ---------- */
    frame_buf[27] = display->voice_seq;
    frame_buf[28] = display->voice_data;

#if BF7613_PROTOCOL_HAS_BYTE29_SEGMENT_MODE
    /* ---------- 数码管 A 段两点使能 (Byt29, V4 only) ---------- */
    frame_buf[BF7613_FRAME_IDX_SEGMENT_MODE] =
        (display->segment_mode.sm1_a_dual_dot ? 0x01 : 0) |
        (display->segment_mode.sm2_a_dual_dot ? 0x02 : 0) |
        (display->segment_mode.sm3_a_dual_dot ? 0x04 : 0) |
        (display->segment_mode.sm6_a_dual_dot ? 0x08 : 0) |
        (display->segment_mode.sm7_a_dual_dot ? 0x10 : 0) |
        (display->segment_mode.sm8_a_dual_dot ? 0x20 : 0);
#endif

    /* ---------- 帧 ID 与结束字节 ---------- */
    frame_buf[BF7613_FRAME_IDX_FRAME_ID] = display->frame_id;
    frame_buf[BF7613_FRAME_IDX_TAIL] = 0x55;

    /* ---------- CRC ---------- */
    uint16_t crc = bf7613_crc16(frame_buf, BF7613_DISPLAY_CRC_LEN);
    frame_buf[BF7613_FRAME_IDX_CRC_L] = crc & 0xFF;
    frame_buf[BF7613_FRAME_IDX_CRC_H] = (crc >> 8) & 0xFF;

    return true;
}
