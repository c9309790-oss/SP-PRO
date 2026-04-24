/**
 * @file opus_player_storage.h
 * @brief Opus 播放器存储管理接口
 */

#ifndef OPUS_PLAYER_STORAGE_H
#define OPUS_PLAYER_STORAGE_H

#include "esp_err.h"
#include "opus_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 文件句柄
 */
typedef void* opus_file_handle_t;

/**
 * @brief 同步结果
 */
typedef struct {
    int added;
    int updated;
    int deleted;
} opus_sync_result_t;

/**
 * @brief 初始化存储模块 (挂载 SPIFFS)
 * @return ESP_OK 成功
 */
esp_err_t opus_player_storage_init(void);

/**
 * @brief 执行同步
 * @param server_url 服务器地址
 * @param result 同步结果输出
 * @return ESP_OK 成功
 */
esp_err_t opus_player_storage_sync(const char *server_url, opus_sync_result_t *result);

/**
 * @brief 获取本地文件列表
 * @param files 文件信息数组
 * @param max_count 最大数量
 * @param count 实际数量输出
 * @return ESP_OK 成功
 */
esp_err_t opus_player_storage_get_list(opus_player_file_info_t *files, size_t max_count, size_t *count);

/**
 * @brief 打开文件
 * @param filename 文件名
 * @param handle 句柄输出
 * @return ESP_OK 成功
 */
esp_err_t opus_player_storage_open(const char *filename, opus_file_handle_t *handle);

/**
 * @brief 读取文件
 * @param handle 文件句柄
 * @param buffer 缓冲区
 * @param size 读取大小
 * @param bytes_read 实际读取大小输出
 * @return ESP_OK 成功
 */
esp_err_t opus_player_storage_read(opus_file_handle_t handle, uint8_t *buffer, size_t size, size_t *bytes_read);

/**
 * @brief 关闭文件
 * @param handle 文件句柄
 * @return ESP_OK 成功
 */
esp_err_t opus_player_storage_close(opus_file_handle_t handle);

/**
 * @brief 获取文件大小
 * @param handle 文件句柄
 * @return 文件大小
 */
size_t opus_player_storage_get_size(opus_file_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // OPUS_PLAYER_STORAGE_H
