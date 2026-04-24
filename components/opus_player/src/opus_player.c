/**
 * @file opus_player.c
 * @brief Opus 播放器组件核心实现
 */

#include "opus_player.h"
#include "opus_player_common.h"
#include "opus_player_audio.h"
#include "opus_player_storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_audio_simple_dec.h"
#include "esp_opus_dec.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <strings.h>
#include <string.h>

// 播放状态
typedef enum {
    OPUS_STATE_IDLE,
    OPUS_STATE_PLAYING,
    OPUS_STATE_PAUSED,
    OPUS_STATE_STOPPED
} opus_player_state_t;

// 播放器上下文
typedef struct {
    opus_player_config_t config;
    opus_player_state_t state;
    TaskHandle_t play_task_handle;
    TaskHandle_t audio_task_handle;
    char current_file[MAX_FILENAME_LEN];
    bool stop_requested;
    bool pause_requested;
    bool decode_done;
    size_t start_prime_bytes;
} opus_player_ctx_t;

static opus_player_ctx_t s_ctx = {0};
static opus_player_t s_instance = {0};

// 前向声明
static void play_task(void *arg);
static void audio_task(void *arg);
static void opus_player_stop_impl(void);

static RingbufHandle_t s_pcm_rb = NULL;
static uint8_t *s_opus_file_buf = NULL;
static size_t s_opus_file_buf_cap = AUDIO_READ_BUF_SIZE;
static uint8_t *s_page_buf = NULL;
static size_t s_page_buf_cap = AUDIO_READ_BUF_SIZE;
static uint8_t *s_packet_buf = NULL;
static size_t s_packet_buf_cap = AUDIO_READ_BUF_SIZE;
static uint8_t *s_decode_buf = NULL;
static size_t s_decode_buf_cap = DECODE_BUF_SIZE;
static uint8_t s_ogg_hdr[27];
static uint8_t s_seg_table[255];
static uint8_t s_silence_buf[SILENCE_BUF_SIZE] = {0};
static uint8_t *s_audio_dma_buf = NULL;
static size_t s_audio_dma_buf_cap = 2048;

static StaticTask_t s_play_task_tcb;
static StackType_t *s_play_task_stack = NULL;
static StaticTask_t s_audio_task_tcb;
static StackType_t *s_audio_task_stack = NULL;

/**
 * @brief 判断字符串是否以指定后缀结尾（忽略大小写）
 */
static bool has_suffix_ci(const char *str, const char *suffix)
{
    if (!str || !suffix) return false;
    size_t slen = strlen(str);
    size_t suflen = strlen(suffix);
    if (slen < suflen) return false;
    return strcasecmp(str + slen - suflen, suffix) == 0;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t off;
} mem_reader_t;

/**
 * @brief 从内存读游标中读取指定长度数据（不足则返回 false）
 */
static bool mem_read_exact(mem_reader_t *r, uint8_t *buf, size_t len)
{
    if (!r || !buf) return false;
    if (r->off + len > r->len) return false;
    memcpy(buf, r->data + r->off, len);
    r->off += len;
    return true;
}

/**
 * @brief 清空 PCM RingBuffer 中的残留数据
 */
static void pcm_rb_drain_all(void)
{
    if (!s_pcm_rb) return;
    while (1) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_pcm_rb, &item_size, 0);
        if (!item) break;
        vRingbufferReturnItem(s_pcm_rb, item);
    }
}

/**
 * @brief 确保缓冲区容量满足需求，优先使用 PSRAM 分配
 */
static esp_err_t ensure_buf_capacity(uint8_t **buf, size_t *cap, size_t need)
{
    if (!buf || !cap) return ESP_ERR_INVALID_ARG;
    if (need == 0) return ESP_ERR_INVALID_SIZE;
    if (*buf && *cap >= need) return ESP_OK;

    size_t new_cap = (need + 4095) & ~4095;
    if (new_cap < AUDIO_READ_BUF_SIZE) new_cap = AUDIO_READ_BUF_SIZE;

    uint8_t *new_buf = (uint8_t *)heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_buf) {
        ESP_LOGI(OPUS_TAG, "Allocated %d bytes in PSRAM", new_cap);
    } else {
        new_buf = (uint8_t *)heap_caps_malloc(new_cap, MALLOC_CAP_8BIT);
        if (new_buf) {
            ESP_LOGW(OPUS_TAG, "Allocated %d bytes in Internal RAM (PSRAM failed/full)", new_cap);
        }
    }
    if (!new_buf) {
        return ESP_ERR_NO_MEM;
    }

    if (*buf) heap_caps_free(*buf);
    *buf = new_buf;
    *cap = new_cap;
    return ESP_OK;
}

/**
 * @brief 确保播放任务栈已分配（必须在 Internal RAM，因为涉及 Flash 操作）
 */
static bool ensure_play_task_stack(void)
{
    if (s_play_task_stack) return true;
    
    // 注意：play_task 中包含 SPI Flash 操作（如读取文件），
    // 而 Flash 操作期间会禁用 Cache，导致 PSRAM 不可访问。
    // 因此，该任务的栈必须分配在 Internal RAM 中，否则会触发断言或崩溃。
    s_play_task_stack = (StackType_t *)heap_caps_malloc(PLAY_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    if (s_play_task_stack) {
        ESP_LOGI(OPUS_TAG,
                 "opus_play_task stack allocated in Internal RAM: %p (%u bytes)",
                 s_play_task_stack,
                 (unsigned)(PLAY_TASK_STACK_SIZE * sizeof(StackType_t)));
    } else {
        ESP_LOGE(OPUS_TAG, "Failed to allocate play task stack in Internal RAM");
    }
    return (s_play_task_stack != NULL);
}

static bool ensure_audio_task_stack(void)
{
    if (s_audio_task_stack) return true;

    s_audio_task_stack = (StackType_t *)heap_caps_malloc(AUDIO_TASK_STACK_SIZE * sizeof(StackType_t),
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_audio_task_stack) {
        ESP_LOGI(OPUS_TAG,
                 "opus_audio_task stack allocated in Internal RAM: %p (%u bytes)",
                 s_audio_task_stack,
                 (unsigned)(AUDIO_TASK_STACK_SIZE * sizeof(StackType_t)));
    } else {
        ESP_LOGE(OPUS_TAG, "Failed to allocate audio task stack in Internal RAM");
    }
    return (s_audio_task_stack != NULL);
}

static esp_err_t reserve_play_task_stack(void)
{
    if (ensure_play_task_stack()) {
        return ESP_OK;
    }

    ESP_LOGE(OPUS_TAG,
             "opus_play_task stack reserve failed during init, bytes=%u",
             (unsigned)(PLAY_TASK_STACK_SIZE * sizeof(StackType_t)));
    return ESP_ERR_NO_MEM;
}

static esp_err_t reserve_audio_task_stack(void)
{
    if (ensure_audio_task_stack()) {
        return ESP_OK;
    }

    ESP_LOGE(OPUS_TAG,
             "opus_audio_task stack reserve failed during init, bytes=%u",
             (unsigned)(AUDIO_TASK_STACK_SIZE * sizeof(StackType_t)));
    return ESP_ERR_NO_MEM;
}

static esp_err_t reserve_audio_dma_buffer(void)
{
    if (s_audio_dma_buf) {
        return ESP_OK;
    }

    s_audio_dma_buf = (uint8_t *)heap_caps_malloc(s_audio_dma_buf_cap,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_audio_dma_buf) {
        ESP_LOGE(OPUS_TAG,
                 "opus_audio dma buffer reserve failed during init, bytes=%u",
                 (unsigned)s_audio_dma_buf_cap);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(OPUS_TAG,
             "opus_audio dma buffer allocated in Internal DMA RAM: %p (%u bytes)",
             s_audio_dma_buf,
             (unsigned)s_audio_dma_buf_cap);
    return ESP_OK;
}

/**
 * @brief 将 Opus 文件一次性读入 RAM（优先 PSRAM），并关闭文件
 */
static esp_err_t load_file_to_ram(const char *filename, size_t *out_size)
{
    if (!filename || !out_size) return ESP_ERR_INVALID_ARG;

    opus_file_handle_t file_handle = NULL;
    esp_err_t ret = opus_player_storage_open(filename, &file_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(OPUS_TAG, "load_file_to_ram open failed file=%s err=%s", filename, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    size_t file_size = opus_player_storage_get_size(file_handle);
    if (file_size == 0) {
        ESP_LOGE(OPUS_TAG, "load_file_to_ram invalid size file=%s size=0", filename);
        opus_player_storage_close(file_handle);
        return ESP_ERR_INVALID_SIZE;
    }

    ret = ensure_buf_capacity(&s_opus_file_buf, &s_opus_file_buf_cap, file_size);
    if (ret != ESP_OK) {
        ESP_LOGE(OPUS_TAG,
                 "load_file_to_ram alloc failed file=%s size=%u err=%s",
                 filename,
                 (unsigned)file_size,
                 esp_err_to_name(ret));
        opus_player_storage_close(file_handle);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < file_size) {
        size_t bytes_read = 0;
        opus_player_storage_read(file_handle, s_opus_file_buf + total, file_size - total, &bytes_read);
        if (bytes_read == 0) {
            ESP_LOGE(OPUS_TAG,
                     "load_file_to_ram read stalled file=%s total=%u size=%u",
                     filename,
                     (unsigned)total,
                     (unsigned)file_size);
            opus_player_storage_close(file_handle);
            return ESP_FAIL;
        }
        total += bytes_read;
    }

    opus_player_storage_close(file_handle);
    *out_size = file_size;
    return ESP_OK;
}

/**
 * @brief 从 PCM RingBuffer 取数据并写入音频设备
 */
static void audio_task(void *arg)
{
    (void)arg;
    const size_t dma_chunk = s_audio_dma_buf_cap;
    bool audio_primed = false;
    if (!s_audio_dma_buf) {
        s_ctx.stop_requested = true;
        s_ctx.audio_task_handle = NULL;
        vTaskDelete(NULL);
    }
    while (!s_ctx.stop_requested) {
        if (s_ctx.pause_requested) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_pcm_rb, &item_size, pdMS_TO_TICKS(100));
        if (item) {
            if (!audio_primed) {
                size_t remaining_prime = s_ctx.start_prime_bytes;
                while (remaining_prime > 0 && !s_ctx.stop_requested) {
                    size_t n = remaining_prime > sizeof(s_silence_buf) ? sizeof(s_silence_buf) : remaining_prime;
                    if (opus_player_audio_write(s_silence_buf, n) != ESP_OK) {
                        s_ctx.stop_requested = true;
                        break;
                    }
                    remaining_prime -= n;
                }
                audio_primed = true;
            }
            size_t off = 0;
            while (off < item_size && !s_ctx.stop_requested) {
                size_t n = item_size - off;
                if (n > dma_chunk) n = dma_chunk;
                memcpy(s_audio_dma_buf, item + off, n);
                if (opus_player_audio_write(s_audio_dma_buf, n) != ESP_OK) {
                    s_ctx.stop_requested = true;
                    break;
                }
                off += n;
            }
            vRingbufferReturnItem(s_pcm_rb, item);
            continue;
        }
        if (s_ctx.decode_done) break;
    }

    while (1) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_pcm_rb, &item_size, 0);
        if (!item) break;
        size_t off = 0;
        while (off < item_size && !s_ctx.stop_requested) {
            size_t n = item_size - off;
            if (n > dma_chunk) n = dma_chunk;
            memcpy(s_audio_dma_buf, item + off, n);
            if (opus_player_audio_write(s_audio_dma_buf, n) != ESP_OK) {
                s_ctx.stop_requested = true;
                break;
            }
            off += n;
        }
        vRingbufferReturnItem(s_pcm_rb, item);
    }
    s_ctx.audio_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 将双声道 16bit PCM 下混为单声道（就地写回）
 */
static size_t downmix_stereo_to_mono_s16(uint8_t *pcm, size_t bytes)
{
    if (!pcm) return 0;
    size_t frames = bytes / 4;
    int16_t *in = (int16_t *)pcm;
    int16_t *out = (int16_t *)pcm;
    for (size_t i = 0; i < frames; i++) {
        int32_t l = in[i * 2 + 0];
        int32_t r = in[i * 2 + 1];
        out[i] = (int16_t)((l + r) / 2);
    }
    return frames * 2;
}

/**
 * @brief 初始化播放器
 */
static esp_err_t opus_player_init_impl(const opus_player_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    // 保存配置
    s_ctx.config = *config;
    
    // Check PSRAM
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(OPUS_TAG, "PSRAM Free Size: %d bytes", free_psram);
    if (free_psram == 0) {
        ESP_LOGW(OPUS_TAG, "PSRAM not available or disabled");
    }

    // 初始化存储
    esp_err_t ret = opus_player_storage_init();
    if (ret != ESP_OK) return ret;
    
    // 初始化音频
    ret = opus_player_audio_init(config);
    if (ret != ESP_OK) return ret;
    
    // 注册 OPUS 解码器
    esp_audio_err_t audio_ret = esp_opus_dec_register();
    if (audio_ret != ESP_AUDIO_ERR_OK && audio_ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(OPUS_TAG, "Failed to register decoder");
        return ESP_FAIL;
    }

    if (!s_opus_file_buf && ensure_buf_capacity(&s_opus_file_buf, &s_opus_file_buf_cap, AUDIO_READ_BUF_SIZE) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_page_buf && ensure_buf_capacity(&s_page_buf, &s_page_buf_cap, AUDIO_READ_BUF_SIZE) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_packet_buf && ensure_buf_capacity(&s_packet_buf, &s_packet_buf_cap, AUDIO_READ_BUF_SIZE) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_decode_buf && ensure_buf_capacity(&s_decode_buf, &s_decode_buf_cap, DECODE_BUF_SIZE) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (reserve_play_task_stack() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (reserve_audio_task_stack() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (reserve_audio_dma_buffer() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_pcm_rb) {
        size_t rb_size = PCM_RB_SIZE;
        uint8_t *rb_storage = (uint8_t *)heap_caps_malloc(rb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rb_storage) {
            rb_storage = (uint8_t *)heap_caps_malloc(rb_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }

        StaticRingbuffer_t *rb_struct = (StaticRingbuffer_t *)heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        
        if (rb_storage && rb_struct) {
            s_pcm_rb = xRingbufferCreateStatic(rb_size, RINGBUF_TYPE_BYTEBUF, rb_storage, rb_struct);
        }
        
        if (!s_pcm_rb) {
             // Fallback: free if allocated but create failed (unlikely), or if malloc failed
             if (rb_storage) heap_caps_free(rb_storage);
             if (rb_struct) heap_caps_free(rb_struct);
             // Fallback to standard creation
             s_pcm_rb = xRingbufferCreate(rb_size, RINGBUF_TYPE_BYTEBUF);
        }

        if (!s_pcm_rb) {
            return ESP_ERR_NO_MEM;
        }
    }
    
    s_ctx.state = OPUS_STATE_IDLE;
    ESP_LOGI(OPUS_TAG, "Opus Player initialized");
    return ESP_OK;
}

/**
 * @brief 初始化播放器（便捷版）
 */
static esp_err_t opus_player_init_ex_impl(const opus_player_config_t *config)
{
    esp_err_t ret = opus_player_init_impl(config);
    if (ret != ESP_OK) return ret;
    return ESP_OK;
}

/**
 * @brief 播放任务（仅支持 Ogg Opus .opus 文件）
 */
static void play_task(void *arg)
{
    const char *filename = (const char *)arg;
    
    if (s_ctx.config.on_play_start) {
        s_ctx.config.on_play_start(filename);
    }
    
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(OPUS_TAG, "opus_play_task high water mark: %u words", (unsigned)watermark);
    
    size_t file_size = 0;
    if (load_file_to_ram(filename, &file_size) != ESP_OK) {
        ESP_LOGE(OPUS_TAG, "load_file_to_ram failed file=%s", filename ? filename : "(null)");
        if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -1);
        goto cleanup;
    }
    ESP_LOGI(OPUS_TAG, "play_task loaded file=%s size=%u bytes", filename ? filename : "(null)", (unsigned)file_size);
    s_ctx.start_prime_bytes = (file_size <= SHORT_OPUS_FILE_BYTES) ?
                              SHORT_AUDIO_START_PRIME_BYTES :
                              AUDIO_START_PRIME_BYTES;
    ESP_LOGI(OPUS_TAG,
             "audio start prime file=%s prime_bytes=%u",
             filename ? filename : "(null)",
             (unsigned)s_ctx.start_prime_bytes);
    
    opus_player_audio_resume(); // 开启 PA
    s_ctx.state = OPUS_STATE_PLAYING;

    if (!has_suffix_ci(filename, ".opus")) {
        if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -2);
        goto cleanup;
    }

    pcm_rb_drain_all();
    s_ctx.decode_done = false;

    size_t packet_len = 0;

    bool got_head = false;
    bool got_tags = false;
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_opus_dec_cfg_t opus_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    opus_cfg.sample_rate = DEFAULT_SAMPLE_RATE;
    // 显式设置帧时长为 120ms (Opus 最大帧长)，以兼容所有可能的帧大小
    // 缓冲区计算: 120ms * 48kHz * 2ch * 2bytes = 23040 bytes < DECODE_BUF_SIZE (32768)
    opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS;

    mem_reader_t reader = {
        .data = s_opus_file_buf,
        .len = file_size,
        .off = 0,
    };

    size_t prebuffered = 0;
    bool audio_started = false;
    bool decode_workload_logged = false;

    while (!s_ctx.stop_requested) {
        if (s_ctx.pause_requested) {
            s_ctx.state = OPUS_STATE_PAUSED;
            opus_player_audio_pause();
            while (s_ctx.pause_requested && !s_ctx.stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (s_ctx.stop_requested) break;
            opus_player_audio_resume();
            s_ctx.state = OPUS_STATE_PLAYING;
        }

        if (!mem_read_exact(&reader, s_ogg_hdr, sizeof(s_ogg_hdr))) {
            ESP_LOGW(OPUS_TAG, "ogg page header EOF file=%s off=%u", filename ? filename : "(null)", (unsigned)reader.off);
            break;
        }
        if (memcmp(s_ogg_hdr, "OggS", 4) != 0 || s_ogg_hdr[4] != 0) {
            ESP_LOGE(OPUS_TAG,
                     "invalid ogg header file=%s off=%u magic=%02X%02X%02X%02X ver=%u",
                     filename ? filename : "(null)",
                     (unsigned)reader.off,
                     s_ogg_hdr[0],
                     s_ogg_hdr[1],
                     s_ogg_hdr[2],
                     s_ogg_hdr[3],
                     s_ogg_hdr[4]);
            if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -4);
            break;
        }

        uint8_t page_segments = s_ogg_hdr[26];
        if (page_segments == 0) {
            continue;
        }
        if (!mem_read_exact(&reader, s_seg_table, page_segments)) {
            break;
        }

        size_t data_size = 0;
        for (uint8_t i = 0; i < page_segments; i++) {
            data_size += s_seg_table[i];
        }

        if (data_size > s_page_buf_cap) {
            if (ensure_buf_capacity(&s_page_buf, &s_page_buf_cap, data_size) != ESP_OK) {
                if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -5);
                break;
            }
        }
        if (data_size > 0 && !mem_read_exact(&reader, s_page_buf, data_size)) {
            break;
        }

        size_t data_off = 0;
        for (uint8_t i = 0; i < page_segments; i++) {
            uint8_t seg_len = s_seg_table[i];
            if (packet_len + seg_len > s_packet_buf_cap) {
                if (ensure_buf_capacity(&s_packet_buf, &s_packet_buf_cap, packet_len + seg_len) != ESP_OK) {
                    if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -6);
                    s_ctx.stop_requested = true;
                    break;
                }
            }

            if (seg_len > 0) {
                if (data_off + seg_len > data_size) {
                    if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -6);
                    s_ctx.stop_requested = true;
                    break;
                }
                memcpy(s_packet_buf + packet_len, s_page_buf + data_off, seg_len);
                packet_len += seg_len;
                data_off += seg_len;
            }

            if (seg_len < 255) {
                if (!got_head && packet_len >= 10 && memcmp(s_packet_buf, "OpusHead", 8) == 0) {
                    uint8_t channels = s_packet_buf[9];
                    if (channels < 1 || channels > 2) {
                        if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -8);
                        s_ctx.stop_requested = true;
                        packet_len = 0;
                        break;
                    }
                    opus_cfg.channel = channels;
                    got_head = true;
                } else if (got_head && !got_tags && packet_len >= 8 && memcmp(s_packet_buf, "OpusTags", 8) == 0) {
                    got_tags = true;
                } else if (got_head && got_tags) {
                    if (!decoder) {
                        esp_audio_simple_dec_cfg_t dec_cfg = {
                            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_RAW_OPUS,
                            .dec_cfg = &opus_cfg,
                            .cfg_size = sizeof(opus_cfg),
                            .use_frame_dec = true,
                        };
                        if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK) {
                            ESP_LOGE(OPUS_TAG,
                                     "esp_audio_simple_dec_open failed file=%s channels=%u frame_dur=%d",
                                     filename ? filename : "(null)",
                                     (unsigned)opus_cfg.channel,
                                     (int)opus_cfg.frame_duration);
                            if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -7);
                            s_ctx.stop_requested = true;
                            packet_len = 0;
                            continue;
                        }
                    }

                    esp_audio_simple_dec_raw_t raw = {
                        .buffer = s_packet_buf,
                        .len = packet_len,
                        .eos = false,
                    };
                    esp_audio_simple_dec_out_t out = {
                        .buffer = s_decode_buf,
                        .len = s_decode_buf_cap,
                    };
                    esp_audio_err_t dret = esp_audio_simple_dec_process(decoder, &raw, &out);
                    if (dret != ESP_AUDIO_ERR_OK && dret != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                        ESP_LOGE(OPUS_TAG,
                                 "esp_audio_simple_dec_process failed file=%s err=%d packet_len=%u",
                                 filename ? filename : "(null)",
                                 (int)dret,
                                 (unsigned)packet_len);
                        s_ctx.stop_requested = true;
                        packet_len = 0;
                        continue;
                    }

                    if (out.decoded_size > 0) {
                        if (!decode_workload_logged) {
                            decode_workload_logged = true;
                            ESP_LOGI(OPUS_TAG,
                                     "opus_play_task first decoded frame, high water mark: %u words",
                                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
                        }
                        if (out.decoded_size > s_decode_buf_cap) {
                            if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -9);
                            s_ctx.stop_requested = true;
                            packet_len = 0;
                            continue;
                        }
                        if ((out.decoded_size & 0x1) != 0) {
                            if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -9);
                            s_ctx.stop_requested = true;
                            packet_len = 0;
                            continue;
                        }
                        size_t pcm_bytes = out.decoded_size;
                        if (opus_cfg.channel == 2) {
                            if ((out.decoded_size & 0x3) != 0) {
                                if (s_ctx.config.on_play_error) s_ctx.config.on_play_error(filename, -9);
                                s_ctx.stop_requested = true;
                                packet_len = 0;
                                continue;
                            }
                            // 强制进行下混，因为硬件输出是单声道的
                            pcm_bytes = downmix_stereo_to_mono_s16(s_decode_buf, out.decoded_size);
                        } else {
                            pcm_bytes = out.decoded_size;
                        }

                        if (!audio_started &&
                            (prebuffered >= PREBUFFER_BYTES || file_size <= SHORT_OPUS_FILE_BYTES)) {
                            if (!ensure_audio_task_stack()) {
                                s_ctx.stop_requested = true;
                            } else if ((s_ctx.audio_task_handle = xTaskCreateStatic(audio_task,
                                                                                    "opus_audio_task",
                                                                                    AUDIO_TASK_STACK_SIZE,
                                                                                    NULL,
                                                                                    6,
                                                                                    s_audio_task_stack,
                                                                                    &s_audio_task_tcb)) != NULL) {
                                audio_started = true;
                                if (file_size <= SHORT_OPUS_FILE_BYTES) {
                                    ESP_LOGI(OPUS_TAG,
                                             "short opus fast-start enabled file=%s size=%u",
                                             filename ? filename : "(null)",
                                             (unsigned)file_size);
                                }
                            } else {
                                s_ctx.stop_requested = true;
                            }
                        }

                        if (!s_ctx.stop_requested) {
                            if (xRingbufferSend(s_pcm_rb, s_decode_buf, pcm_bytes, pdMS_TO_TICKS(200)) != pdTRUE) {
                                s_ctx.stop_requested = true;
                            } else if (!audio_started) {
                                prebuffered += pcm_bytes;
                            }
                        }
                    }
                }
                packet_len = 0;
            }
        }
    }

    s_ctx.decode_done = true;
    if (!audio_started && !s_ctx.stop_requested) {
        if (ensure_audio_task_stack() &&
            (s_ctx.audio_task_handle = xTaskCreateStatic(audio_task,
                                                         "opus_audio_task",
                                                         AUDIO_TASK_STACK_SIZE,
                                                         NULL,
                                                         6,
                                                         s_audio_task_stack,
                                                         &s_audio_task_tcb)) != NULL) {
            audio_started = true;
        }
    }

    if (audio_started) {
        int timeout = 50;
        while (s_ctx.audio_task_handle && timeout-- > 0 && !s_ctx.stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    if (!s_ctx.stop_requested) {
        for (int i = 0; i < 8; i++) {
            opus_player_audio_write(s_silence_buf, sizeof(s_silence_buf));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_ctx.config.on_play_finish) s_ctx.config.on_play_finish(filename);
    }

    if (decoder) {
        esp_audio_simple_dec_close(decoder);
    }
cleanup:
    opus_player_audio_pause(); // 关闭 PA
    s_ctx.state = OPUS_STATE_IDLE;
    s_ctx.play_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t opus_player_play_impl(const char *filename, bool blocking)
{
    if (s_ctx.state != OPUS_STATE_IDLE && s_ctx.state != OPUS_STATE_STOPPED) {
        // 如果正在播放，先停止
        opus_player_stop_impl();
    }
    
    s_ctx.stop_requested = false;
    s_ctx.pause_requested = false;
    s_ctx.decode_done = false;
    strncpy(s_ctx.current_file, filename, MAX_FILENAME_LEN - 1);
    s_ctx.current_file[MAX_FILENAME_LEN - 1] = '\0';
    
    // 立即设置状态为播放中，避免任务启动延迟导致的竞态条件
    s_ctx.state = OPUS_STATE_PLAYING;

    if (!ensure_play_task_stack()) {
        s_ctx.state = OPUS_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }

    s_ctx.play_task_handle = xTaskCreateStatic(play_task,
                                               "opus_play_task",
                                               PLAY_TASK_STACK_SIZE,
                                               (void *)s_ctx.current_file,
                                               5,
                                               s_play_task_stack,
                                               &s_play_task_tcb);
    if (!s_ctx.play_task_handle) {
        s_ctx.state = OPUS_STATE_IDLE;
        return ESP_FAIL;
    }
    
    // 如果是阻塞模式，等待播放结束
    if (blocking) {
        while (s_ctx.state == OPUS_STATE_PLAYING || s_ctx.state == OPUS_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    
    return ESP_OK;
}

static void opus_player_pause_impl(void)
{
    if (s_ctx.state == OPUS_STATE_PLAYING) {
        s_ctx.pause_requested = true;
    }
}

static void opus_player_resume_impl(void)
{
    if (s_ctx.state == OPUS_STATE_PAUSED) {
        s_ctx.pause_requested = false;
    }
}

static void opus_player_stop_impl(void)
{
    if (s_ctx.state != OPUS_STATE_IDLE && s_ctx.play_task_handle) {
        s_ctx.stop_requested = true;
        // 等待任务结束
        int timeout = 20;
        while (s_ctx.play_task_handle && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static bool opus_player_is_playing_impl(void)
{
    return (s_ctx.state == OPUS_STATE_PLAYING || s_ctx.state == OPUS_STATE_PAUSED);
}

static esp_err_t opus_player_set_volume_impl(int volume)
{
    return opus_player_audio_set_volume(volume);
}

static int opus_player_get_volume_impl(void)
{
    return opus_player_audio_get_volume();
}

static esp_err_t opus_player_sync_impl(void)
{
    if (!s_ctx.config.server_url) return ESP_ERR_INVALID_STATE;
    
    opus_sync_result_t result = {0};
    esp_err_t ret = opus_player_storage_sync(s_ctx.config.server_url, &result);
    
    if (ret == ESP_OK) {
        ESP_LOGI(OPUS_TAG, "Sync result: +%d, ~%d, -%d", result.added, result.updated, result.deleted);
    }
    return ret;
}

static esp_err_t opus_player_get_file_list_impl(opus_player_file_info_t *files, size_t max_count, size_t *count)
{
    return opus_player_storage_get_list(files, max_count, count);
}

const opus_player_t *opus_player_get_instance(void)
{
    static bool initialized = false;
    if (!initialized) {
        // 初始化函数指针
        s_instance.init = opus_player_init_impl;
        s_instance.init_ex = opus_player_init_ex_impl;
        s_instance.play = opus_player_play_impl;
        s_instance.pause = opus_player_pause_impl;
        s_instance.resume = opus_player_resume_impl;
        s_instance.stop = opus_player_stop_impl;
        s_instance.is_playing = opus_player_is_playing_impl;
        s_instance.set_volume = opus_player_set_volume_impl;
        s_instance.get_volume = opus_player_get_volume_impl;
        s_instance.sync = opus_player_sync_impl;
        s_instance.get_file_list = opus_player_get_file_list_impl;
        initialized = true;
    }
    return &s_instance;
}
