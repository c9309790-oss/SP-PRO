#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "spiffs.h"
#include "nvs_flash.h"
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <esp_system.h>
#include "circular_buff.h"

#ifdef KLM_FLASH_LOG_ENABLE
#define MAX_LOG_FILE_SIZE 1024 * 60  // 60 KB
#define FLASH_LOG_RING_BUF_SIZE (1024*5)
#define FLASH_LOG_BUF_SIZE (1024)
#define FLASH_LOG_TASK_STACK_SIZE_DEFAULT 4096
#define FLASH_LOG_TASK_STACK_SIZE_TEST    3072
#define FLASH_LOG_TASK_STACK_SIZE         FLASH_LOG_TASK_STACK_SIZE_TEST

static const char *TAG = "spiffs";
static FILE* log_file = NULL;
static unsigned long log_file_size=0,log_file_backup_size=0;
static char flash_log_circular_buff[FLASH_LOG_RING_BUF_SIZE]={0};
static char flash_log_data_buff[FLASH_LOG_BUF_SIZE]={0};
static CircularBuffer flash_log_ring_buf;
static void flash_log_task(void *pvParameters);
static void check_log_file_size();

static void flash_log_task(void *pvParameters) {
    int len;
    long size_write = 0;
    UBaseType_t watermark;
    bool workload_logged = false;

    (void)pvParameters;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "flash_log_task start, stack=%u bytes, high_water=%u words",
             (unsigned)FLASH_LOG_TASK_STACK_SIZE,
             (unsigned)watermark);

    log_file = fopen(LOG_FILE_BACKUP_PATH, "r");
    if (log_file != NULL) {
        log_file_backup_size = ftell(log_file);
        fclose(log_file);
    }
 
    log_file = fopen(LOG_FILE_PATH, "a+");
    if (log_file == NULL) {
        ESP_LOGE(TAG, "Failed to open log file for writing");
        return;
    }

    log_file_size = ftell(log_file);
    ESP_LOGI(TAG, "Log file opened successfully. Current size: %ld", log_file_size);

    while (1) {
        if (!circular_buffer_is_empty(&flash_log_ring_buf)) {
            len = circular_buffer_read(&flash_log_ring_buf, flash_log_data_buff, sizeof(flash_log_data_buff)-1);
            if (len > 0) {
                check_log_file_size();  // 写入前检查日志文件大小
                flash_log_data_buff[len] = '\0';
                size_write = fprintf(log_file, "%s", flash_log_data_buff);
                if (size_write > 0) {
                    log_file_size += size_write;
                    printf("log_file size %ld\n", log_file_size);
                    if (!workload_logged) {
                        workload_logged = true;
                        watermark = uxTaskGetStackHighWaterMark(NULL);
                        ESP_LOGI(TAG,
                                 "flash_log_task first write complete, high_water=%u words",
                                 (unsigned)watermark);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to write to log file. Return value: %ld", size_write);
                    watermark = uxTaskGetStackHighWaterMark(NULL);
                    ESP_LOGW(TAG,
                             "flash_log_task write failure, high_water=%u words",
                             (unsigned)watermark);
                    remove_log_file();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // 降低任务轮询频率，避免空转
    }
}

void remove_log_file(void)
{
    FILE* file = fopen(LOG_FILE_BACKUP_PATH, "r");
    if (file != NULL) {
        fclose(file);
        remove(LOG_FILE_BACKUP_PATH);
        ESP_LOGW(TAG, "Remove %s",LOG_FILE_BACKUP_PATH);
        log_file_size = 0;
    }
    file = fopen(LOG_FILE_PATH, "r");
    if (file != NULL) {
        fclose(file);
        remove(LOG_FILE_PATH);
        ESP_LOGW(TAG, "Remove %s",LOG_FILE_PATH);
        log_file_backup_size = 0;
    }
    // 重新创建当前日志文件
    log_file = fopen(LOG_FILE_PATH, "a+");
    if (log_file == NULL) {
        ESP_LOGE(TAG, "Failed to open new log file for writing");
    }
}
void rotate_log_file()
{
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
    FILE* file = fopen(LOG_FILE_BACKUP_PATH, "r");
    if (file != NULL) {
        fclose(file);
        // 删除旧的备份日志文件
        remove(LOG_FILE_BACKUP_PATH);
        log_file_backup_size=0;
    }
    
    // 将当前日志文件重命名为备份文件
    rename(LOG_FILE_PATH, LOG_FILE_BACKUP_PATH);
    log_file_backup_size=log_file_size;
    // 创建新的当前日志文件
    log_file = fopen(LOG_FILE_PATH, "a+");
    if (log_file == NULL) {
        ESP_LOGE(TAG, "Failed to open new log file for writing");
    }
    log_file_size=0;
}

static void check_log_file_size() {
    if (log_file_size >= MAX_LOG_FILE_SIZE) {
        ESP_LOGI(TAG, "Log file size reached limit, rotating log file");
        rotate_log_file();
    }
}
unsigned long get_log_file_size(void)
{ 
    return log_file_size;
}

unsigned long get_log_file_backup_size(void)
{ 
    return log_file_backup_size;
}
void read_spiffs_on_off_from_nvs(char *spiffs_on_off) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("spiffs_config", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }
    
    size_t name_len = 0;
    err = nvs_get_str(nvs_handle, "spiffs_on_off", NULL, &name_len);
    if (err == ESP_OK && name_len > 0) {
        nvs_get_str(nvs_handle, "spiffs_on_off", spiffs_on_off, &name_len);
    } else {
        spiffs_on_off[0] = '\0';
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "spiffs info read on_off:%s", spiffs_on_off);
}

void save_spiffs_on_off_to_nvs(char *spiffs_on_off) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("spiffs_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, "spiffs_on_off", spiffs_on_off);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) setting spiffs name in NVS", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) committing changes to NVS", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "spiffs on_off saved to NVS %s", spiffs_on_off);
}

void print_files_in_spiffs(void) {
    DIR* dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open /spiffs directory");
        return;
    }

    struct dirent* entry;
    char file_path[265];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) { // Regular file
            snprintf(file_path, sizeof(file_path), "/spiffs/%s", entry->d_name);

            FILE* file = fopen(file_path, "r");
            if (file == NULL) {
                ESP_LOGE(TAG, "Failed to open file: %s", file_path);
                continue;
            }

            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            if (file_size == -1) {
                ESP_LOGE(TAG, "Failed to determine file size: %s", file_path);
                fclose(file);
                continue;
            }
            fclose(file);

            ESP_LOGI(TAG, "File: %s, Size: %ld bytes", entry->d_name, file_size); // 打印文件名和文件大小
        }
    }
    closedir(dir);
}
char spiffs_on_off_str[5]="",spiffs_on_off_flag=0;
void init_spiffs(void)
{
    
    read_spiffs_on_off_from_nvs(spiffs_on_off_str);
    if(spiffs_on_off_str[0]==0)
    {
        save_spiffs_on_off_to_nvs("off");
        read_spiffs_on_off_from_nvs(spiffs_on_off_str);
    }


    if(memcmp(spiffs_on_off_str, "off", 3)==0)
    {
        spiffs_on_off_flag=0;
        ESP_LOGI(TAG, "Spiffs log is off");
        return;
    }
    
    spiffs_on_off_flag=1;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "log",
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    // 执行 SPIFFS 文件系统一致性检查
    ESP_LOGI(TAG, "Performing SPIFFS_check().");
    ret = esp_spiffs_check(conf.partition_label);// 当 SPIFFS 挂载异常时，可调用该接口进行修复检查
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG, "SPIFFS_check() successful");
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    print_files_in_spiffs();

    // 初始化用于写日志的环形缓冲区
    init_circular_buffer(&flash_log_ring_buf,sizeof(flash_log_circular_buff),flash_log_circular_buff,"flash_log");

    if (xTaskCreate(flash_log_task, "flash_log_task", FLASH_LOG_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create Flash log task, stack=%u bytes",
                 (unsigned)FLASH_LOG_TASK_STACK_SIZE);
        return;
    }
    ESP_LOGI(TAG,
             "Flash log task created, stack=%u bytes",
             (unsigned)FLASH_LOG_TASK_STACK_SIZE);
}
#endif

int custom_log_write(const char* format, va_list args)
{
    // 先格式化日志内容到本地缓冲区
    static char log_message[1024];  // 复用静态缓冲区，避免频繁申请内存
    int len=0;
    len=vsnprintf(log_message, sizeof(log_message), format, args);

#ifdef KLM_FLASH_LOG_ENABLE
    if (spiffs_on_off_flag) {
        // 将日志写入 SPIFFS 缓冲区
        if (log_file != NULL && len>0) {
            circular_buffer_write(&flash_log_ring_buf, log_message, len);
        }
    }
#endif

    // 同时输出到标准终端
    len = vprintf(format, args);
    return len;
}



