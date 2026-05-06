/**
 * @file opus_player_storage.c
 * @brief Opus 播放器存储管理实现
 */

#include "opus_player_storage.h"
#include "opus_player_common.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "mbedtls/md5.h"
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

static const char *TAG = "opus_storage";

#define OPUS_PARTITION_LABEL   "opus_data"
#define OPUS_BASE_PATH         "/opus"
#define MD5_CACHE_FILE        "/opus/.md5cache"
#define MAX_CACHED_FILES      MAX_OPUS_FILES

// 存储状态
static EXT_RAM_BSS_ATTR struct {
    bool initialized;
    struct {
        char name[MAX_FILENAME_LEN];
        char md5[33];
    } md5_cache[MAX_CACHED_FILES];
    int cache_count;
} s_storage = {0};

static void log_storage_bootstrap_state(void)
{
    struct stat st = {0};
    const char *probe = OPUS_BASE_PATH "/key.opus";
    if (stat(probe, &st) == 0) {
        ESP_LOGI(TAG, "Storage probe: key.opus present size=%u", (unsigned)st.st_size);
    } else {
        ESP_LOGE(TAG, "Storage probe: key.opus missing path=%s errno=%d", probe, errno);
    }

    DIR *dir = opendir(OPUS_BASE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Storage probe: opendir failed path=%s errno=%d", OPUS_BASE_PATH, errno);
        return;
    }

    size_t opus_count = 0;
    char sample0[MAX_FILENAME_LEN] = {0};
    char sample1[MAX_FILENAME_LEN] = {0};
    char sample2[MAX_FILENAME_LEN] = {0};
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 5 && strcasecmp(entry->d_name + len - 5, ".opus") == 0) {
            if (opus_count == 0) {
                strncpy(sample0, entry->d_name, sizeof(sample0) - 1);
            } else if (opus_count == 1) {
                strncpy(sample1, entry->d_name, sizeof(sample1) - 1);
            } else if (opus_count == 2) {
                strncpy(sample2, entry->d_name, sizeof(sample2) - 1);
            }
            opus_count++;
        }
    }
    closedir(dir);

    ESP_LOGI(TAG,
             "Storage probe: opus_count=%u sample0=%s sample1=%s sample2=%s",
             (unsigned)opus_count,
             sample0[0] ? sample0 : "-",
             sample1[0] ? sample1 : "-",
             sample2[0] ? sample2 : "-");
}

// 内部文件信息结构体
typedef struct {
    char name[MAX_FILENAME_LEN];
    uint32_t size;
    char md5[33];
} opus_internal_file_info_t;

// 加载 MD5 缓存
static void load_md5_cache(void)
{
    s_storage.cache_count = 0;
    FILE *f = fopen(MD5_CACHE_FILE, "r");
    if (!f) return;
    
    char line[MD5_LINE_BUFFER_SIZE];
    while (fgets(line, sizeof(line), f) && s_storage.cache_count < MAX_CACHED_FILES) {
        char *sep = strchr(line, '|');
        if (sep) {
            *sep = '\0';
            strncpy(s_storage.md5_cache[s_storage.cache_count].name, line, MAX_FILENAME_LEN - 1);
            strncpy(s_storage.md5_cache[s_storage.cache_count].md5, sep + 1, 32);
            s_storage.md5_cache[s_storage.cache_count].md5[32] = '\0';
            char *nl = strchr(s_storage.md5_cache[s_storage.cache_count].md5, '\n');
            if (nl) *nl = '\0';
            s_storage.cache_count++;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d cached MD5 entries", s_storage.cache_count);
}

// 保存 MD5 缓存
static void save_md5_cache(void)
{
    FILE *f = fopen(MD5_CACHE_FILE, "w");
    if (!f) return;
    
    for (int i = 0; i < s_storage.cache_count; i++) {
        fprintf(f, "%s|%s\n", s_storage.md5_cache[i].name, s_storage.md5_cache[i].md5);
    }
    fclose(f);
}

// 从缓存获取 MD5
static const char* get_cached_md5(const char *filename)
{
    for (int i = 0; i < s_storage.cache_count; i++) {
        if (strcmp(s_storage.md5_cache[i].name, filename) == 0) {
            return s_storage.md5_cache[i].md5;
        }
    }
    return NULL;
}

// 更新缓存中的 MD5
static void update_cached_md5(const char *filename, const char *md5)
{
    for (int i = 0; i < s_storage.cache_count; i++) {
        if (strcmp(s_storage.md5_cache[i].name, filename) == 0) {
            strncpy(s_storage.md5_cache[i].md5, md5, 32);
            save_md5_cache();
            return;
        }
    }
    if (s_storage.cache_count < MAX_CACHED_FILES) {
        strncpy(s_storage.md5_cache[s_storage.cache_count].name, filename, MAX_FILENAME_LEN - 1);
        strncpy(s_storage.md5_cache[s_storage.cache_count].md5, md5, 32);
        s_storage.cache_count++;
        save_md5_cache();
    }
}

// 从缓存中删除 MD5
static void remove_cached_md5(const char *filename)
{
    for (int i = 0; i < s_storage.cache_count; i++) {
        if (strcmp(s_storage.md5_cache[i].name, filename) == 0) {
            for (int j = i; j < s_storage.cache_count - 1; j++) {
                s_storage.md5_cache[j] = s_storage.md5_cache[j + 1];
            }
            s_storage.cache_count--;
            save_md5_cache();
            return;
        }
    }
}

esp_err_t opus_player_storage_init(void)
{
    if (s_storage.initialized) return ESP_OK;
    
    ESP_LOGI(TAG, "Initializing Opus storage...");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = OPUS_BASE_PATH,
        .partition_label = OPUS_PARTITION_LABEL,
        .max_files = 10,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    
    size_t total = 0, used = 0;
    if (esp_spiffs_info(OPUS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: %d/%d KB", used / 1024, total / 1024);
    }
    
    load_md5_cache();
    log_storage_bootstrap_state();
    s_storage.initialized = true;
    return ESP_OK;
}

/**
 * @brief 检查 URL 是否为 HTTP（不允许 HTTPS）
 */
static bool is_http_url(const char *url)
{
    if (!url) return false;
    return strncmp(url, "http://", 7) == 0;
}

/**
 * @brief 检查文件名是否安全（避免路径穿越/目录分隔符）
 */
static bool is_safe_filename(const char *filename)
{
    if (!filename || filename[0] == '\0') return false;
    for (const char *p = filename; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (*p == '/' || *p == '\\') return false;
        if (!(isalnum(c) || *p == '_' || *p == '-' || *p == '.')) return false;
    }
    return true;
}

/**
 * @brief 获取文件大小
 */
static esp_err_t get_file_size_by_path(const char *path, size_t *size_out)
{
    if (!path || !size_out) return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    *size_out = (size_t)st.st_size;
    return ESP_OK;
}

/**
 * @brief 计算指定路径文件的 MD5
 */
static esp_err_t get_local_md5_by_path(const char *path, char md5_out[33])
{
    if (!path || !md5_out) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);

    uint8_t buffer[MD5_BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        mbedtls_md5_update(&ctx, buffer, bytes_read);
    }
    fclose(f);

    uint8_t digest[16];
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);

    for (int i = 0; i < 16; i++) {
        sprintf(md5_out + i * 2, "%02x", digest[i]);
    }
    md5_out[32] = '\0';
    return ESP_OK;
}

/**
 * @brief 原子替换文件：dst 存在则先备份，再用 src 覆盖
 */
static esp_err_t atomic_replace_file(const char *src_path, const char *dst_path, const char *bak_path)
{
    if (!src_path || !dst_path || !bak_path) return ESP_ERR_INVALID_ARG;

    struct stat st;
    bool dst_exists = (stat(dst_path, &st) == 0);

    remove(bak_path);

    if (dst_exists) {
        if (rename(dst_path, bak_path) != 0) {
            return ESP_FAIL;
        }
    }

    if (rename(src_path, dst_path) != 0) {
        if (dst_exists) {
            rename(bak_path, dst_path);
        }
        return ESP_FAIL;
    }

    if (dst_exists) {
        remove(bak_path);
    }
    return ESP_OK;
}

/**
 * @brief 通过 HTTP 下载文件到临时文件并校验完整性后替换旧文件
 */
static esp_err_t download_file_http_atomic(const char *server_url, const opus_internal_file_info_t *info)
{
    if (!server_url || !info) return ESP_ERR_INVALID_ARG;
    if (!is_http_url(server_url)) return ESP_ERR_INVALID_ARG;
    if (!is_safe_filename(info->name)) return ESP_ERR_INVALID_ARG;
    if (strlen(info->md5) != 32) return ESP_ERR_INVALID_ARG;

    // Allocate buffers on heap to save stack
    char *url = opus_malloc(URL_BUFFER_SIZE);
    char *final_path = opus_malloc(MAX_PATH_LEN);
    char *part_path = opus_malloc(MAX_PATH_LEN);
    char *bak_path = opus_malloc(MAX_PATH_LEN);
    
    if (!url || !final_path || !part_path || !bak_path) {
        opus_free(url); opus_free(final_path); opus_free(part_path); opus_free(bak_path);
        return ESP_ERR_NO_MEM;
    }

    snprintf(url, URL_BUFFER_SIZE, "%s/api/download/%s", server_url, info->name);
    snprintf(final_path, MAX_PATH_LEN, "%s/%s", OPUS_BASE_PATH, info->name);
    snprintf(part_path, MAX_PATH_LEN, "%s/.%s.%s.part", OPUS_BASE_PATH, info->name, info->md5);
    snprintf(bak_path, MAX_PATH_LEN, "%s/.%s.bak", OPUS_BASE_PATH, info->name);

    esp_err_t ret = ESP_FAIL;
    size_t part_size = 0;
    if (get_file_size_by_path(part_path, &part_size) == ESP_OK) {
        if (info->size > 0 && part_size > info->size) {
            remove(part_path);
            part_size = 0;
        } else if (info->size > 0 && part_size == info->size) {
            char md5_now[33] = {0};
            if (get_local_md5_by_path(part_path, md5_now) == ESP_OK && strcasecmp(md5_now, info->md5) == 0) {
                ret = atomic_replace_file(part_path, final_path, bak_path);
                goto cleanup;
            }
            remove(part_path);
            part_size = 0;
        }
    }

    FILE *f = fopen(part_path, part_size > 0 ? "ab" : "wb");
    if (!f) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = HTTP_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        ret = ESP_FAIL;
        goto cleanup;
    }

    char range_value[48];
    if (part_size > 0) {
        snprintf(range_value, sizeof(range_value), "bytes=%u-", (unsigned)part_size);
        esp_http_client_set_header(client, "Range", range_value);
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        fclose(f);
        ret = ESP_FAIL;
        goto cleanup;
    }

    (void)esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (part_size > 0 && status_code == 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(f);
        remove(part_path);
        ret = ESP_FAIL; 
        goto cleanup;
    }

    if (!(status_code == 200 || status_code == 206)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(f);
        ret = ESP_FAIL;
        goto cleanup;
    }

    uint8_t *buffer = (uint8_t *)opus_malloc(HTTP_BUFFER_SIZE);
    if (!buffer) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(f);
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    int64_t start_time = esp_timer_get_time();
    size_t total_bytes = part_size;
    size_t content_len = esp_http_client_get_content_length(client);
    if (content_len > 0) content_len += part_size;
    
    while (1) {
        int rlen = esp_http_client_read(client, (char *)buffer, (int)HTTP_BUFFER_SIZE);
        if (rlen < 0) {
            ret = ESP_FAIL;
            break;
        }
        if (rlen == 0) {
            ret = ESP_OK;
            break;
        }
        if (fwrite(buffer, 1, rlen, f) != (size_t)rlen) {
            ret = ESP_FAIL;
            break;
        }
        total_bytes += rlen;
        
        int64_t now = esp_timer_get_time();
        if (now - start_time > 1000000) { // 每秒打印一次
            float speed = (float)(total_bytes - part_size) / ((now - start_time) / 1000000.0f) / 1024.0f;
            if (content_len > 0) {
                float progress = (float)total_bytes / content_len * 100.0f;
                ESP_LOGI(TAG, "Downloading %s: %.1f%% (%.1f KB/s)", info->name, progress, speed);
            } else {
                ESP_LOGI(TAG, "Downloading %s: %u bytes (%.1f KB/s)", info->name, (unsigned)total_bytes, speed);
            }
        }
    }
    
    int64_t end_time = esp_timer_get_time();
    float final_speed = (float)(total_bytes - part_size) / ((end_time - start_time) / 1000000.0f) / 1024.0f;
    ESP_LOGI(TAG, "Download %s finished: %u bytes, avg speed: %.1f KB/s", info->name, (unsigned)total_bytes, final_speed);

    opus_free(buffer);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(f);

    if (ret != ESP_OK) {
        goto cleanup;
    }

    size_t final_part_size = 0;
    if (get_file_size_by_path(part_path, &final_part_size) != ESP_OK) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (info->size > 0 && final_part_size != (size_t)info->size) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    char md5_now[33] = {0};
    if (get_local_md5_by_path(part_path, md5_now) != ESP_OK) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (strcasecmp(md5_now, info->md5) != 0) {
        remove(part_path);
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = atomic_replace_file(part_path, final_path, bak_path);

cleanup:
    opus_free(url);
    opus_free(final_path);
    opus_free(part_path);
    opus_free(bak_path);
    return ret;
}

static esp_err_t get_remote_file_info(const char *server_url, const char *filename, opus_internal_file_info_t *info)
{
    char url[URL_BUFFER_SIZE];
    snprintf(url, sizeof(url), "%s/api/info/%s", server_url, filename);
    
    char *buffer = opus_malloc(INFO_BUFFER_SIZE);
    if (!buffer) return ESP_ERR_NO_MEM;
    
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000, 
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        opus_free(buffer);
        return ESP_FAIL;
    }
    
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open connection for info: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        opus_free(buffer);
        return ret;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        if (esp_http_client_get_status_code(client) != 200) {
             ESP_LOGE(TAG, "Failed to fetch headers or status not 200");
             esp_http_client_close(client);
             esp_http_client_cleanup(client);
             opus_free(buffer);
             return ESP_FAIL;
        }
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "Get info failed, status=%d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        opus_free(buffer);
        return ESP_FAIL;
    }
    
    int total_read = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buffer + total_read, INFO_BUFFER_SIZE - 1 - total_read);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Error reading info data");
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }
        total_read += read_len;
        if (total_read >= INFO_BUFFER_SIZE - 1) {
            ESP_LOGW(TAG, "Info buffer full");
            break;
        }
    }
    buffer[total_read] = '\0';
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (ret != ESP_OK && total_read == 0) {
        opus_free(buffer);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse info JSON: %s", buffer);
        opus_free(buffer);
        return ESP_FAIL;
    }
    opus_free(buffer); 
    
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *size = cJSON_GetObjectItem(root, "size");
    cJSON *md5 = cJSON_GetObjectItem(root, "md5");
    
    if (cJSON_IsString(name)) strncpy(info->name, name->valuestring, MAX_FILENAME_LEN - 1);
    if (cJSON_IsNumber(size)) info->size = size->valueint;
    if (cJSON_IsString(md5)) strncpy(info->md5, md5->valuestring, 32);
    
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t list_remote_files(const char *server_url, opus_internal_file_info_t *files, size_t max_files, size_t *count)
{
    char url[URL_BUFFER_SIZE];
    snprintf(url, sizeof(url), "%s/api/list", server_url);
    
    char *buffer = opus_malloc(HTTP_BUFFER_SIZE);
    if (!buffer) return ESP_ERR_NO_MEM;
    
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,
        .buffer_size = HTTP_BUFFER_SIZE,  // 增加接收缓冲区大小
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        opus_free(buffer);
        return ESP_FAIL;
    }
    
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        opus_free(buffer);
        return ret;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers");
        esp_http_client_cleanup(client);
        opus_free(buffer);
        return ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
        esp_http_client_cleanup(client);
        opus_free(buffer);
        return ESP_FAIL;
    }
    
    int total_read = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buffer + total_read, HTTP_BUFFER_SIZE - 1 - total_read);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Error reading data");
            break;
        }
        if (read_len == 0) {
            break;
        }
        total_read += read_len;
        if (total_read >= HTTP_BUFFER_SIZE - 1) {
            ESP_LOGW(TAG, "Buffer full, truncated");
            break;
        }
    }
    buffer[total_read] = '\0';
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    *count = 0;
    cJSON *root = cJSON_Parse(buffer);
    if (root) {
        cJSON *files_array = cJSON_GetObjectItem(root, "files");
        if (cJSON_IsArray(files_array)) {
            int n = cJSON_GetArraySize(files_array);
            for (int i = 0; i < n && *count < max_files; i++) {
                cJSON *item = cJSON_GetArrayItem(files_array, i);
                cJSON *name = cJSON_GetObjectItem(item, "name");
                cJSON *size = cJSON_GetObjectItem(item, "size");
                
                if (cJSON_IsString(name) && cJSON_IsNumber(size)) {
                    strncpy(files[*count].name, name->valuestring, MAX_FILENAME_LEN - 1);
                    files[*count].size = size->valueint;
                    (*count)++;
                }
            }
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        ESP_LOGD(TAG, "Response content: %s", buffer);
        opus_free(buffer);
        return ESP_FAIL;
    }
    opus_free(buffer);
    return ESP_OK;
}

static bool local_file_exists(const char *filename)
{
    char filepath[MAX_PATH_LEN];
    snprintf(filepath, sizeof(filepath), "%s/%s", OPUS_BASE_PATH, filename);
    struct stat st;
    return (stat(filepath, &st) == 0);
}

static esp_err_t get_local_md5(const char *filename, char *md5_out)
{
    char filepath[MAX_PATH_LEN];
    snprintf(filepath, sizeof(filepath), "%s/%s", OPUS_BASE_PATH, filename);
    return get_local_md5_by_path(filepath, md5_out);
}

static esp_err_t delete_file(const char *filename)
{
    char filepath[MAX_PATH_LEN];
    snprintf(filepath, sizeof(filepath), "%s/%s", OPUS_BASE_PATH, filename);
    if (remove(filepath) != 0) return ESP_FAIL;
    remove_cached_md5(filename);
    ESP_LOGI(TAG, "Deleted: %s", filename);
    return ESP_OK;
}

esp_err_t opus_player_storage_sync(const char *server_url, opus_sync_result_t *result)
{
    if (!s_storage.initialized) return ESP_ERR_INVALID_STATE;
    if (!is_http_url(server_url)) return ESP_ERR_INVALID_ARG;
    
    ESP_LOGI(TAG, "Starting sync with %s...", server_url);
    
    size_t max_files = MAX_OPUS_FILES;
    opus_internal_file_info_t *all_remote_files = opus_calloc(max_files, sizeof(opus_internal_file_info_t));
    if (!all_remote_files) return ESP_ERR_NO_MEM;

    size_t remote_count = 0;
    esp_err_t ret = list_remote_files(server_url, all_remote_files, max_files, &remote_count);
    if (ret != ESP_OK) {
        opus_free(all_remote_files);
        return ret;
    }
    
    ESP_LOGI(TAG, "Remote files: %d", remote_count);
    
    for (size_t i = 0; i < remote_count; i++) {
        const char *filename = all_remote_files[i].name;
        opus_internal_file_info_t info = {0};

        if (strlen(filename) <= 5 || strcasecmp(filename + strlen(filename) - 5, ".opus") != 0) {
            continue;
        }
        
        if (get_remote_file_info(server_url, filename, &info) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get info for: %s", filename);
            continue;
        }

        if (!is_safe_filename(info.name) || strlen(info.md5) != 32) {
            ESP_LOGW(TAG, "Skip invalid file info: %s", filename);
            continue;
        }
        
        bool need_download = false;
        bool existed_before = local_file_exists(filename);
        if (existed_before) {
            const char *cached_md5 = get_cached_md5(filename);
            if (cached_md5) {
                if (strcmp(cached_md5, info.md5) != 0) need_download = true;
            } else {
                char local_md5[33] = {0};
                if (get_local_md5(filename, local_md5) == ESP_OK) {
                    update_cached_md5(filename, local_md5);
                    if (strcmp(local_md5, info.md5) != 0) need_download = true;
                } else {
                    need_download = true;
                }
            }
        } else {
            need_download = true;
        }
        
        if (need_download) {
            if (download_file_http_atomic(server_url, &info) == ESP_OK) {
                update_cached_md5(filename, info.md5);
                if (existed_before) result->updated++;
                else result->added++;
            }
        }
    }

    // 删除本地多余文件
    DIR *dir = opendir(OPUS_BASE_PATH);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strlen(entry->d_name) > 5 && strcasecmp(entry->d_name + strlen(entry->d_name) - 5, ".opus") == 0) {
                bool found = false;
                for (size_t i = 0; i < remote_count; i++) {
                    if (strlen(all_remote_files[i].name) <= 5 || strcasecmp(all_remote_files[i].name + strlen(all_remote_files[i].name) - 5, ".opus") != 0) {
                        continue;
                    }
                    if (strcmp(entry->d_name, all_remote_files[i].name) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (delete_file(entry->d_name) == ESP_OK) {
                        result->deleted++;
                    }
                }
            }
        }
        closedir(dir);
    }
    
    opus_free(all_remote_files);
    return ESP_OK;
}

esp_err_t opus_player_storage_get_list(opus_player_file_info_t *files, size_t max_count, size_t *count)
{
    if (!s_storage.initialized) return ESP_ERR_INVALID_STATE;
    
    *count = 0;
    DIR *dir = opendir(OPUS_BASE_PATH);
    if (!dir) return ESP_OK;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *count < max_count) {
        if (strlen(entry->d_name) > 5 && strcasecmp(entry->d_name + strlen(entry->d_name) - 5, ".opus") == 0) {
            strncpy(files[*count].name, entry->d_name, 63);
            
            char filepath[MAX_PATH_LEN];
            snprintf(filepath, sizeof(filepath), "%s/%s", OPUS_BASE_PATH, entry->d_name);
            struct stat st;
            if (stat(filepath, &st) == 0) {
                files[*count].size = st.st_size;
            }
            (*count)++;
        }
    }
    closedir(dir);
    return ESP_OK;
}

esp_err_t opus_player_storage_open(const char *filename, opus_file_handle_t *handle)
{
    if (!s_storage.initialized || !filename || !handle) return ESP_ERR_INVALID_ARG;
    
    char filepath[MAX_PATH_LEN];
    snprintf(filepath, sizeof(filepath), "%s/%s", OPUS_BASE_PATH, filename);
    
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open failed file=%s path=%s errno=%d", filename, filepath, errno);
        return ESP_ERR_NOT_FOUND;
    }
    
    *handle = (opus_file_handle_t)f;
    return ESP_OK;
}

esp_err_t opus_player_storage_read(opus_file_handle_t handle, uint8_t *buffer, size_t size, size_t *bytes_read)
{
    if (!handle || !buffer) return ESP_ERR_INVALID_ARG;
    FILE *f = (FILE *)handle;
    *bytes_read = fread(buffer, 1, size, f);
    if (*bytes_read == 0 && ferror(f)) {
        ESP_LOGE(TAG, "read failed requested=%u errno=%d", (unsigned)size, errno);
    }
    return ESP_OK;
}

esp_err_t opus_player_storage_close(opus_file_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    FILE *f = (FILE *)handle;
    fclose(f);
    return ESP_OK;
}

size_t opus_player_storage_get_size(opus_file_handle_t handle)
{
    if (!handle) return 0;
    FILE *f = (FILE *)handle;
    long current = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, current, SEEK_SET);
    return (size_t)size;
}
