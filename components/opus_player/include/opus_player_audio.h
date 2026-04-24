/**
 * @file opus_player_audio.h
 * @brief Opus 播放器音频驱动接口
 */

#ifndef OPUS_PLAYER_AUDIO_H
#define OPUS_PLAYER_AUDIO_H

#include "esp_err.h"
#include "opus_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音频驱动
 * @param config 配置参数
 * @return ESP_OK 成功
 */
esp_err_t opus_player_audio_init(const opus_player_config_t *config);

/**
 * @brief 设置音量
 * @param volume 音量 (0-100)
 * @return ESP_OK 成功
 */
esp_err_t opus_player_audio_set_volume(int volume);

/**
 * @brief 获取当前音量
 * @return 音量 (0-100)
 */
int opus_player_audio_get_volume(void);

/**
 * @brief 写入音频数据
 * @param data 数据指针
 * @param len 数据长度
 * @return ESP_OK 成功
 */
esp_err_t opus_player_audio_write(const uint8_t *data, size_t len);

/**
 * @brief 暂停音频输出 (关闭 PA)
 */
void opus_player_audio_pause(void);

/**
 * @brief 恢复音频输出 (开启 PA)
 */
void opus_player_audio_resume(void);

/**
 * @brief 停止音频输出
 */
esp_err_t opus_player_audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif // OPUS_PLAYER_AUDIO_H
