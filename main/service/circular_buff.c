#include "circular_buff.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "circular_buff";

// 初始化环形缓冲区及互斥锁
void init_circular_buffer(CircularBuffer *cb,int size,char *buff,char* name) {
    cb->head = 0;
    cb->tail = 0;
    cb->buffer = buff;
    cb->name = name;
    cb->size = size;
    cb->mutex = xSemaphoreCreateMutex(); // 用于保护并发读写
}


int circular_buffer_is_full(CircularBuffer *cb) {
    return ((cb->head + 1) % cb->size) == cb->tail;
}

int circular_buffer_is_empty(CircularBuffer *cb) {
    return cb->head == cb->tail;
}

void circular_buffer_write(CircularBuffer *cb, const char *data, int len) {
    if (xSemaphoreTake(cb->mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < len; i++) {
            if (!circular_buffer_is_full(cb)) {
                cb->buffer[cb->head] = data[i];
                cb->head = (cb->head + 1) % cb->size;
            } else {
                ESP_LOGE(TAG, "%s Buffer Overflow len=%d data[0]=%02X",cb->name,len, data[0]);
                break;
            }
        }
        xSemaphoreGive(cb->mutex);
    }
    else
    {
        ESP_LOGE(TAG, "%s TX mutex err len=%d data[0]=%02X",cb->name,len, data[0]);
    }
}

void circular_buffer_write_over(CircularBuffer *cb, const char *data, int len) {
    if (xSemaphoreTake(cb->mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < len; i++) {
            if (circular_buffer_is_full(cb)) {
                // 缓冲区满时覆盖最旧数据，推进 tail
                cb->tail = (cb->tail + 1) % cb->size;
            }
            cb->buffer[cb->head] = data[i];
            cb->head = (cb->head + 1) % cb->size;
        }
        xSemaphoreGive(cb->mutex);
    }
}

int circular_buffer_read(CircularBuffer *cb, char *data, int len) {
    int count = 0;
    while (!circular_buffer_is_empty(cb) && count < len) {
        data[count] = cb->buffer[cb->tail];
        cb->tail = (cb->tail + 1) % cb->size;
        count++;
    }
    return count;
}
