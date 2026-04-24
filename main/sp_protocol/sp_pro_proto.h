#ifndef __SP_PRO_PROTO_H__
#define __SP_PRO_PROTO_H__

#include <stdint.h>
#include <stdbool.h>

/* Supplier-only BF7613 key/display demo.
 * 0: normal product firmware
 * 1: standalone membrane/segment inspection demo */
#define SUPPLIER_HMI_DEMO_MODE 0

/* ===== 协议版本选择 =====
 * V3: 旧协议，无 Byt29 A段两点控制
 * V4: 新协议，新增 Byt29 用于 SM1/2/3/6/7/8 的 A段两点使能
 */
#define BF7613_PROTOCOL_V3 3U
#define BF7613_PROTOCOL_V4 4U

#ifndef BF7613_PROTOCOL_VERSION
#define BF7613_PROTOCOL_VERSION BF7613_PROTOCOL_V3
#endif

#if (BF7613_PROTOCOL_VERSION == BF7613_PROTOCOL_V4)
#define BF7613_PROTOCOL_HAS_BYTE29_SEGMENT_MODE 1U
#define BF7613_DISPLAY_FRAME_LEN 34U
#define BF7613_DISPLAY_CRC_LEN 32U
#define BF7613_FRAME_IDX_SEGMENT_MODE 29U
#define BF7613_FRAME_IDX_FRAME_ID 30U
#define BF7613_FRAME_IDX_TAIL 31U
#define BF7613_FRAME_IDX_CRC_L 32U
#define BF7613_FRAME_IDX_CRC_H 33U
#elif (BF7613_PROTOCOL_VERSION == BF7613_PROTOCOL_V3)
#define BF7613_PROTOCOL_HAS_BYTE29_SEGMENT_MODE 0U
#define BF7613_DISPLAY_FRAME_LEN 33U
#define BF7613_DISPLAY_CRC_LEN 31U
#define BF7613_FRAME_IDX_FRAME_ID 29U
#define BF7613_FRAME_IDX_TAIL 30U
#define BF7613_FRAME_IDX_CRC_L 31U
#define BF7613_FRAME_IDX_CRC_H 32U
#else
#error "Unsupported BF7613_PROTOCOL_VERSION"
#endif

/* ===== 协议帧长度 ===== */
enum {
    BF7613_KEY_FRAME_LEN = 8,
    BF7613_KEY_NAME_COUNT = 10,
};


/* ===== 亮度与段码定义 ===== */
typedef enum {
    WHITE_LIGHT_OFF  = 0,  /* 00: 熄灭 */
    WHITE_LIGHT_HALF = 1,  /* 01: 半亮 */
    WHITE_LIGHT_FULL = 3,  /* 11: 全亮 */
} white_light_level_t;

typedef enum {
    SM_POS_1,
    SM_POS_2,
    SM_POS_3,
    SM_POS_4,
    SM_POS_5,
    SM_POS_6,
    SM_POS_7,
    SM_POS_8,
    SM_POS_9,
    SM_POS_10,
    SM_POS_11,
    SM_POS_12,
    SM_POS_COUNT
} seg_position_t;

typedef enum {
    DISP_DOT_LEFT_WEIGHT,
    DISP_DOT_RIGHT_WEIGHT,
    DISP_DOT_COUNT
} seg_dot_t;

typedef enum {
    SEG_POS_GROUP_LARGE,
    SEG_POS_GROUP_SMALL,
} seg_pos_group_t;

typedef enum {
    SEG_CHAR_0 = 0,
    SEG_CHAR_1,
    SEG_CHAR_2,
    SEG_CHAR_3,
    SEG_CHAR_4,
    SEG_CHAR_5,
    SEG_CHAR_6,
    SEG_CHAR_7,
    SEG_CHAR_8,
    SEG_CHAR_9,
    SEG_CHAR_H,
    SEG_CHAR_BLANK,
    SEG_CHAR_COUNT
} seg7_char_t;

typedef struct {
    bool dot[DISP_DOT_COUNT];
} disp_dot_state_t;

typedef struct {
    bool sm1_a_dual_dot : 1;
    bool sm2_a_dual_dot : 1;
    bool sm3_a_dual_dot : 1;
    bool sm6_a_dual_dot : 1;
    bool sm7_a_dual_dot : 1;
    bool sm8_a_dual_dot : 1;
    bool                : 1;
    bool                : 1;
} disp_segment_mode_byte29_t;

/* ===== 显示帧各字节块 ===== */
typedef struct {
    uint8_t digit[SEG_CHAR_COUNT];
} disp_segment_block_t;

typedef struct {
    white_light_level_t k1_icon  : 2;
    white_light_level_t k1_text  : 2;
    white_light_level_t k2_icon  : 2;
    white_light_level_t k2_text  : 2;
} disp_key_light_byte14_t;

typedef struct {
    white_light_level_t k3_icon  : 2;
    white_light_level_t k3_text  : 2;
    white_light_level_t k4_icon  : 2;
    white_light_level_t k4_text  : 2;
} disp_key_light_byte15_t;

typedef struct {
    white_light_level_t k5_icon  : 2;
    white_light_level_t k5_text  : 2;
    white_light_level_t k6_icon  : 2;
    white_light_level_t k7_icon  : 2;
} disp_key_light_byte16_t;

typedef struct {
    white_light_level_t k8_icon   : 2;
    white_light_level_t k9_icon   : 2;
    white_light_level_t k10_white : 2;
    white_light_level_t k10_blue  : 2;
} disp_key_light_byte17_t;

typedef struct {
    disp_key_light_byte14_t byte14;
    disp_key_light_byte15_t byte15;
    disp_key_light_byte16_t byte16;
    disp_key_light_byte17_t byte17;
} disp_key_light_block_t;

typedef struct {
    bool p1 : 1;
    bool p2 : 1;
    bool p3 : 1;
    bool p4 : 1;
    bool p5 : 1;
    bool s1 : 1;
    bool s2 : 1;
    bool s3 : 1;
} disp_icon_byte18_t;

typedef struct {
    bool s4  : 1;
    bool s5  : 1;
    bool s6  : 1;
    bool s7  : 1;
    bool s8  : 1;
    bool s9  : 1;
    bool s10 : 1;
    bool s11 : 1;
} disp_icon_byte19_t;

typedef struct {
    white_light_level_t l1 : 2;
    white_light_level_t l2 : 2;
    white_light_level_t l3 : 2;
    white_light_level_t l4 : 2;
} disp_icon_byte20_t;

typedef struct {
    disp_icon_byte18_t byte18;
    disp_icon_byte19_t byte19;
    disp_icon_byte20_t byte20;
} disp_icon_block_t;

typedef struct {
    bool k1_blink : 1;
    bool k2_blink : 1;
    bool k3_blink : 1;
    bool k4_blink : 1;
    bool k5_blink : 1;
    bool k6_blink : 1;
    bool k7_blink : 1;
    bool k8_blink : 1;
} disp_blink_byte23_t;

typedef struct {
    bool k9_blink     : 1;
    bool k10_blink    : 1;
    bool p1_red_blink : 1;
    bool p2_red_blink : 1;
    bool p3_red_blink : 1;
    bool p4_red_blink : 1;
    bool p5_red_blink : 1;
    bool              : 1;
} disp_blink_byte24_t;

typedef struct {
    bool s1_blink : 1;
    bool s2_blink : 1;
    bool s3_blink : 1;
    bool s4_blink : 1;
    bool s5_blink : 1;
    bool s6_blink : 1;
    bool s7_blink : 1;
    bool s8_blink : 1;
} disp_blink_byte25_t;

typedef struct {
    bool s9_blink  : 1;
    bool s10_blink : 1;
    bool s11_blink : 1;
    bool l1_blink  : 1;
    bool l2_blink  : 1;
    bool l3_blink  : 1;
    bool l4_blink  : 1;
    bool           : 1;
} disp_blink_byte26_t;

typedef struct {
    disp_blink_byte23_t byte23;
    disp_blink_byte24_t byte24;
    disp_blink_byte25_t byte25;
    disp_blink_byte26_t byte26;
} disp_blink_block_t;

#pragma pack(push, 1)

typedef struct {
    /* 帧尾之前的帧序号:
     * - V3: Byt29
     * - V4: Byt30
     */
    uint8_t frame_id;

    /* Byt2-Byt13: 数码管段码 */
    disp_segment_block_t segment;
    disp_dot_state_t dots;

    /* V4 Byt29: A 段两点使能；V3 下忽略 */
    disp_segment_mode_byte29_t segment_mode;

    /* Byt14-Byt17: 按键灯状态 */
    disp_key_light_block_t keys;

    /* Byt18-Byt20: 图标状态 */
    disp_icon_block_t icons;

    /* Byt21-Byt22: 进度条 */
    uint8_t q1_gauge;
    uint8_t q2_gauge;

    /* Byt23-Byt26: 闪烁控制 */
    disp_blink_block_t blink;

    /* Byt27-Byt28: 语音控制 */
    uint8_t voice_seq;
    uint8_t voice_data;
} disp_element_t;

#pragma pack(pop)

/* ===== 按键事件 ===== */
typedef struct {
    uint16_t key_mask;
    uint8_t frame_id;
} bf7613_key_event_t;

/* ===== 编解码接口 ===== */
uint8_t bf7613_get_seg7_char_pos(seg_position_t pos, seg7_char_t ch);

bool bf7613_build_display_frame(const disp_element_t *display,
                                uint8_t *frame_buf,
                                uint16_t buf_len);

bool bf7613_parse_key_frame(const uint8_t *buf,
                            uint16_t len,
                            bf7613_key_event_t *event);

uint16_t bf7613_crc16(const uint8_t *data, uint16_t len);

#endif /* __SP_PRO_PROTO_H__ */
