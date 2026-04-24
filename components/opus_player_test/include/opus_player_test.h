/**
 * @file opus_player_test.h
 * @brief Opus 播放器功能测试组件接口
 */

#ifndef OPUS_PLAYER_TEST_H
#define OPUS_PLAYER_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Opus 播放器功能测试
 * 
 * 该函数会创建独立的 FreeRTOS 任务来运行播放器测试逻辑，
 * 包括初始化、同步文件和循环播放。
 */
void start_opus_player_test(void);

#ifdef __cplusplus
}
#endif

#endif // OPUS_PLAYER_TEST_H
