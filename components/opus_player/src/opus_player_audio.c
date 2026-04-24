/**
 * @file opus_player_audio.c
 * @brief Opus 播放器音频驱动实现
 */

#include "opus_player_audio.h"
#include "opus_player_common.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "opus_audio";

// 驱动状态
static struct {
    bool initialized;
    int pa_gpio;
    uint32_t sample_rate;
    i2c_master_bus_handle_t i2c_bus;
    esp_codec_dev_handle_t codec_dev;
} s_audio = {0};

/**
 * @brief 初始化 PA GPIO
 */
static esp_err_t init_pa_gpio(int gpio_num)
{
    if (gpio_num < 0) {
        return ESP_OK;
    }
    
    gpio_reset_pin(gpio_num);
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_drive_capability(gpio_num, GPIO_DRIVE_CAP_3);
    gpio_set_level(gpio_num, 0);
    
    ESP_LOGI(TAG, "PA GPIO %d initialized", gpio_num);
    return ESP_OK;
}

/**
 * @brief 初始化 I2C 总线
 */
static esp_err_t init_i2c(const opus_player_config_t *cfg)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = cfg->i2c_sda_io,
        .scl_io_num = cfg->i2c_scl_io,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    
    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_audio.i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C initialized (SDA=%d, SCL=%d)", cfg->i2c_sda_io, cfg->i2c_scl_io);
    return ESP_OK;
}

/**
 * @brief 初始化 ES8311 Codec
 */
static esp_err_t init_codec(const opus_player_config_t *cfg)
{
    // 创建 I2S 通道
    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
    
    // I2S 标准模式配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_audio.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = cfg->i2s_mclk_io,
            .bclk = cfg->i2s_bclk_io,
            .ws = cfg->i2s_ws_io,
            .dout = cfg->i2s_dout_io,
            .din = cfg->i2s_din_io,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    
    // 创建 I2S 数据接口
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }
    
    // 创建 I2C 控制接口
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = cfg->i2c_addr,
        .bus_handle = s_audio.i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        return ESP_FAIL;
    }
    
    // 创建 GPIO 接口
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "Failed to create GPIO interface");
        return ESP_FAIL;
    }
    
    // ES8311 配置
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .pa_reverted = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        .master_mode = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
    };
    
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (es8311_dev == NULL) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return ESP_FAIL;
    }
    
    // 创建 Codec 设备
    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = data_if,
    };
    s_audio.codec_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (s_audio.codec_dev == NULL) {
        ESP_LOGE(TAG, "Failed to create codec device");
        return ESP_FAIL;
    }
    
    // 打开 Codec
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = s_audio.sample_rate,
        .mclk_multiple = 0,
    };
    
    esp_err_t ret = esp_codec_dev_open(s_audio.codec_dev, &sample_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open codec: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "ES8311 initialized (%dHz, 16bit, mono)", (int)s_audio.sample_rate);
    return ESP_OK;
}

esp_err_t opus_player_audio_init(const opus_player_config_t *config)
{
    if (s_audio.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    s_audio.pa_gpio = config->pa_gpio;
    s_audio.sample_rate = DEFAULT_SAMPLE_RATE;
    
    // 初始化 PA GPIO
    esp_err_t ret = init_pa_gpio(config->pa_gpio);
    if (ret != ESP_OK) return ret;
    
    // 初始化 I2C
    ret = init_i2c(config);
    if (ret != ESP_OK) return ret;
    
    // 初始化 Codec
    ret = init_codec(config);
    if (ret != ESP_OK) return ret;

    int volume = config->volume;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    esp_codec_dev_set_out_vol(s_audio.codec_dev, volume);
    
    s_audio.initialized = true;
    ESP_LOGI(TAG, "Audio driver initialized successfully");
    return ESP_OK;
}

esp_err_t opus_player_audio_set_volume(int volume)
{
    if (!s_audio.initialized || s_audio.codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // 将 0-100 映射到浮点数 0.0-100.0
    return esp_codec_dev_set_out_vol(s_audio.codec_dev, volume);
}

int opus_player_audio_get_volume(void)
{
    if (!s_audio.initialized || s_audio.codec_dev == NULL) {
        return 0;
    }
    int vol = 0;
    esp_codec_dev_get_out_vol(s_audio.codec_dev, &vol);
    return vol;
}

esp_err_t opus_player_audio_write(const uint8_t *data, size_t len)
{
    if (!s_audio.initialized || s_audio.codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_write(s_audio.codec_dev, (void *)data, len);
}

void opus_player_audio_pause(void)
{
    if (s_audio.pa_gpio >= 0) {
        gpio_set_level(s_audio.pa_gpio, 0); // 关闭 PA
    }
}

void opus_player_audio_resume(void)
{
    if (s_audio.pa_gpio >= 0) {
        gpio_set_level(s_audio.pa_gpio, 1); // 开启 PA
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t opus_player_audio_stop(void)
{
    opus_player_audio_pause();
    return ESP_OK;
}
