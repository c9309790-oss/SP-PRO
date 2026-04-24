/**
 * @file opus_player_common.h
 * @brief Opus 播放器组件内部公共定义
 */

#ifndef OPUS_PLAYER_COMMON_H
#define OPUS_PLAYER_COMMON_H

#include "esp_log.h"
#include "opus_player.h"
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

// 日志 TAG
#define OPUS_TAG "opus_player"

// 最大支持文件数
#define MAX_OPUS_FILES 300

// 文件名最大长度
#define MAX_FILENAME_LEN 64

// 默认音频采样率
#define DEFAULT_SAMPLE_RATE 48000

// 音频缓冲区大小
#define AUDIO_READ_BUF_SIZE 16384

// PCM RingBuffer 大小
#define PCM_RB_SIZE (16 * 1024)

// 解码缓冲区大小
#define DECODE_BUF_SIZE (32 * 1024)

// 预缓冲大小
#define PREBUFFER_BYTES (8 * 1024)

// 超短提示音文件阈值：小于该大小时不等待完整预缓冲，尽快启动输出
#define SHORT_OPUS_FILE_BYTES 2048

// 输出启动预热静音：先送一小段 0 PCM，避免超短音频被功放/DAC 起振阶段吞掉
#define AUDIO_START_PRIME_BYTES 2048

// 超短文件使用更长的启动预热，防止真正的提示音样本落在输出链路的起振阶段
#define SHORT_AUDIO_START_PRIME_BYTES 8192

// 静音数据缓冲区大小
#define SILENCE_BUF_SIZE 1024

// 播放任务栈大小
#define PLAY_TASK_STACK_SIZE 12288

// 音频输出任务栈大小
#define AUDIO_TASK_STACK_SIZE 2048

// HTTP 缓冲区大小
#define HTTP_BUFFER_SIZE 32768

// 最大路径长度
#define MAX_PATH_LEN 280

// URL 缓冲区大小
#define URL_BUFFER_SIZE 256

// 信息 JSON 缓冲区大小
#define INFO_BUFFER_SIZE 2048

// MD5 读取缓冲区大小
#define MD5_BUFFER_SIZE 1024

// MD5 缓存行缓冲区大小
#define MD5_LINE_BUFFER_SIZE 128

static inline void *opus_malloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) return ptr;
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static inline void *opus_calloc(size_t n, size_t size) {
    void *ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) return ptr;
    return heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
}

static inline void opus_free(void *ptr) {
    if (ptr) heap_caps_free(ptr);
}

#ifdef __cplusplus
}
#endif

#endif // OPUS_PLAYER_COMMON_H
