# Opus Player 组件使用文档

**版本**: 1.0.0  
**适用平台**: ESP32 (S3/C3 等系列)  
**依赖组件**: `espressif/esp_audio_codec`, `esp_http_client`

## 1. 简介

`opus_player` 是一个高度封装的 Opus 音频播放组件，专为 ESP32 设计。它集成了音频解码、I2S 输出、文件存储管理（基于 SPIFFS）以及与远程服务器的文件同步功能。

### 核心特性
*   **开箱即用**: 仅需简单的配置即可启动播放。
*   **远程同步**: 内置 HTTP 客户端，可从指定服务器同步 Opus 文件。
*   **断点续传**: 支持大文件分块下载和完整性校验（MD5）。
*   **内存优化**: 针对 ESP32 的内存结构进行了优化，优先使用 PSRAM，关键任务使用 Internal RAM。
*   **格式支持**: 专为 Opus (`.opus`) 格式优化，支持标准 Ogg 封装。

---

## 2. 快速开始

### 2.1 添加组件依赖

在你的项目 `main/CMakeLists.txt` 中添加对 `opus_player` 的依赖。

**示例 `main/CMakeLists.txt`**:
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        opus_player          # 核心依赖
        nvs_flash            # 存储初始化依赖
        esp_wifi             # 网络功能依赖
        esp_event            # 事件循环依赖
)

# [可选] 创建 SPIFFS 分区镜像
# 如果你希望在固件烧录阶段就将音频文件预置到设备中（无需首次联网下载），可以添加以下配置。
# 参数 1: 分区名称 (必须与 partitions.csv 中的 Name 一致，例如 opus_data)
# 参数 2: 本地音频文件目录 (例如 ../server/opus_files)
# 参数 3: FLASH_IN_PROJECT (自动随 idf.py flash 烧录)
# spiffs_create_partition_image(opus_data "../server/opus_files" FLASH_IN_PROJECT)

# 完整示例:
# 创建 SPIFFS 分区镜像
# 将 ../server/opus_files 目录下的内容打包到 opus_data 分区
# FLASH_IN_PROJECT 会在执行 idf.py flash 时自动烧录该分区
spiffs_create_partition_image(opus_data "../server/opus_files" FLASH_IN_PROJECT)
```

如果你的项目结构比较复杂，或者 `opus_player` 位于自定义的组件目录中，请确保在项目根目录的 `CMakeLists.txt` 中设置了正确的 `EXTRA_COMPONENT_DIRS`。

**示例项目根目录 `CMakeLists.txt`**:
```cmake
cmake_minimum_required(VERSION 3.16)

# 如果 opus_player 在项目根目录下的 components 文件夹中，通常不需要额外配置。
# 如果在其他位置（例如 ../shared_components），请添加：
# set(EXTRA_COMPONENT_DIRS "../shared_components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_audio_project)
```

### 2.2 基础示例代码

```c
#include "opus_player.h"

void app_main(void)
{
    // 1. 获取播放器实例
    const opus_player_t *player = opus_player_get_instance();
    if (!player) {
        printf("Failed to get player instance\n");
        return;
    }

    // 2. 配置参数
    opus_player_config_t config = {
        // I2S 音频接口配置 (请根据实际板卡修改引脚)
        .i2s_mclk_io = 38,
        .i2s_bclk_io = 47,
        .i2s_ws_io = 21,
        .i2s_dout_io = 13,
        .i2s_din_io = 12,
        
        // I2C 控制接口 (用于配置 Codec 芯片，如 ES8311)
        .i2c_sda_io = 1,
        .i2c_scl_io = 2,
        .i2c_addr = 0x30,

        // 功放使能引脚
        .pa_gpio = 15,

        // 远程文件服务器地址 (可选)
        .server_url = "http://192.168.1.100:8080",
        
        // 初始音量
        .volume = 60
    };

    // 3. 初始化播放器
    if (player->init_ex(&config) != ESP_OK) {
        printf("Player init failed\n");
        return;
    }

    // 4. (可选) 同步远程文件
    // 需确保 WiFi 已连接
    player->sync();

    // 5. 播放文件
    // 阻塞模式播放 "welcome.opus"
    player->play("welcome.opus", true);
}
```

---

## 3. 详细 API 说明

所有 API 均通过 `opus_player_get_instance()` 获取的函数指针结构体调用。

### 3.1 初始化

#### `init_ex`
高级初始化函数，会自动完成硬件初始化、文件系统挂载、音量设置等步骤。

```c
esp_err_t (*init_ex)(const opus_player_config_t *config);
```

### 3.2 播放控制

#### `play`
播放指定的 Opus 文件。

```c
esp_err_t (*play)(const char *filename, bool blocking);
```
*   `filename`: 文件名（不包含路径，例如 `song.opus`）。
*   `blocking`: 
    *   `true`: 阻塞当前任务直到播放结束。
    *   `false`: 立即返回，后台异步播放。

#### `pause` / `resume` / `stop`
基本的播放控制。

```c
void (*pause)(void);   // 暂停
void (*resume)(void);  // 恢复
void (*stop)(void);    // 停止
```

### 3.3 音量控制

#### `set_volume` / `get_volume`
设置或获取当前音量（0-100）。

```c
esp_err_t (*set_volume)(int volume);
int (*get_volume)(void);
```

### 3.4 文件管理

#### `sync`
触发与远程服务器的文件同步。
*   该操作会比较本地与服务器的文件列表（基于 MD5）。
*   自动下载新文件或更新文件。
*   自动删除服务器上不存在的本地文件。

```c
esp_err_t (*sync)(void);
```

#### `get_file_list`
获取本地已存储的文件列表。

```c
esp_err_t (*get_file_list)(opus_player_file_info_t *files, size_t max_count, size_t *count);
```

---

## 4. 配置结构体详解

`opus_player_config_t` 定义了硬件引脚和软件行为。

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `i2c_sda_io` | `int` | I2C 数据引脚 (用于 Codec 控制) |
| `i2c_scl_io` | `int` | I2C 时钟引脚 |
| `i2s_mclk_io` | `int` | I2S 主时钟 (MCLK) |
| `i2s_bclk_io` | `int` | I2S 位时钟 (BCLK) |
| `i2s_ws_io` | `int` | I2S 字选/帧同步 (LRCK/WS) |
| `i2s_dout_io` | `int` | I2S 数据输出 (DIN/SDOUT) |
| `pa_gpio` | `int` | 外部功放使能引脚 (高电平有效) |
| `server_url` | `char*` | 文件服务器地址 (无结尾斜杠) |
| `volume` | `int` | 默认音量 (0-100) |
| `on_play_start` | `func` | 播放开始回调 |
| `on_play_finish` | `func` | 播放结束回调 |
| `on_play_error` | `func` | 播放出错回调 |

---

## 5. 常见问题

### 5.1 内存不足 (ESP_ERR_NO_MEM)
Opus 解码需要较大的内存。
*   请确保启用了 PSRAM (SPIRAM)。
*   组件会自动尝试在 PSRAM 中分配大块缓冲区。
*   如果使用 `play` 任务栈溢出，请检查 `opus_player_common.h` 中的 `PLAY_TASK_STACK_SIZE`。

### 5.2 播放无声
1.  检查 `pa_gpio` 是否正确配置。
2.  检查 `volume` 设置。
3.  确保 Codec 芯片（如 ES8311）初始化成功。

### 5.3 同步失败
1.  确保 WiFi 已连接且信号良好。
2.  确保 `server_url` 配置正确且服务器可访问。
3.  检查服务端是否返回了正确的 JSON 格式（参考服务端文档）。
