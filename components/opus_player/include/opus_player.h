/**
 * @file opus_player.h
 * @brief Opus 播放器组件统一接口
 */

#ifndef OPUS_PLAYER_H
#define OPUS_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 类型定义
// ============================================================================

/**
 * @brief Opus 播放器配置结构体
 */
typedef struct {
    // I2C 配置
    int i2c_sda_io;         ///< I2C SDA 引脚
    int i2c_scl_io;         ///< I2C SCL 引脚
    uint8_t i2c_addr;       ///< I2C 地址（默认 0x30）
    
    // I2S 配置
    int i2s_mclk_io;        ///< I2S MCLK 引脚
    int i2s_bclk_io;        ///< I2S BCLK 引脚
    int i2s_ws_io;          ///< I2S WS 引脚
    int i2s_dout_io;        ///< I2S DOUT 引脚
    int i2s_din_io;         ///< I2S DIN 引脚
    
    // PA 配置
    int pa_gpio;            ///< PA 使能引脚（-1 表示不使用）
    
    // 服务器配置
    const char *server_url; ///< 服务器完整地址 (例如 "http://192.168.18.85:8080")

    // 播放参数
    int volume;             ///< 初始音量 (0-100)
    
    // 回调函数（可选）
    void (*on_play_start)(const char *filename);
    void (*on_play_finish)(const char *filename);
    void (*on_play_error)(const char *filename, int error_code);
} opus_player_config_t;

/**
 * @brief Opus 文件信息
 */
typedef struct {
    char name[64];          ///< 文件名
    uint32_t size;          ///< 文件大小
} opus_player_file_info_t;

/**
 * @brief Opus 播放器句柄结构体
 */
typedef struct opus_player_s opus_player_t;

struct opus_player_s {
    /**
     * @brief 初始化播放器
     * @param config 配置参数
     * @return ESP_OK 成功
     */
    esp_err_t (*init)(const opus_player_config_t *config);

    /**
     * @brief 初始化播放器（便捷版）
     * 
     * 一次完成初始化、音量设置、文件列表获取，并准备好播放。
     * 
     * @param config 配置参数
     * @return ESP_OK 成功
     */
    esp_err_t (*init_ex)(const opus_player_config_t *config);

    /**
     * @brief 播放指定文件
     * @param filename 文件名
     * @param blocking 是否阻塞直到播放结束 (true: 阻塞, false: 非阻塞)
     * @return ESP_OK 成功
     */
    esp_err_t (*play)(const char *filename, bool blocking);

    /**
     * @brief 暂停播放
     */
    void (*pause)(void);

    /**
     * @brief 恢复播放
     */
    void (*resume)(void);

    /**
     * @brief 停止播放
     */
    void (*stop)(void);

    /**
     * @brief 检查是否正在播放
     * @return true: 正在播放或暂停, false: 空闲或停止
     */
    bool (*is_playing)(void);

    /**
     * @brief 设置音量
     * @param volume 音量 (0-100)
     * @return ESP_OK 成功
     */
    esp_err_t (*set_volume)(int volume);

    /**
     * @brief 获取当前音量
     * @return 音量 (0-100)
     */
    int (*get_volume)(void);

    /**
     * @brief 与服务器同步文件
     * @return ESP_OK 成功
     */
    esp_err_t (*sync)(void);

    /**
     * @brief 获取本地文件列表
     * @param files 文件信息数组
     * @param max_count 最大数量
     * @param count 实际数量输出
     * @return ESP_OK 成功
     */
    esp_err_t (*get_file_list)(opus_player_file_info_t *files, size_t max_count, size_t *count);
};

// 获取单例实例
const opus_player_t *opus_player_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // OPUS_PLAYER_H
