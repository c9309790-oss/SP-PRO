#ifndef CIRCULAR_BUFF_H
#define CIRCULAR_BUFF_H
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
typedef struct {
    char *name;
    char *buffer;
    volatile int head;
    volatile int tail;
    int size;
    SemaphoreHandle_t mutex; // ????????
} CircularBuffer;



void init_circular_buffer(CircularBuffer *cb,int size,char *buff,char* name);
int circular_buffer_is_full(CircularBuffer *cb);
int circular_buffer_is_empty(CircularBuffer *cb);
void circular_buffer_write(CircularBuffer *cb, const char *data, int len);
void circular_buffer_write_over(CircularBuffer *cb, const char *data, int len);
int circular_buffer_read(CircularBuffer *cb, char *data, int len);

#endif /* CIRCULAR_BUFF_H */


