#ifndef SPIFFS_H
#define SPIFFS_H

#include <stdarg.h>

#define LOG_FILE_PATH "/spiffs/log.txt"
#define LOG_FILE_BACKUP_PATH "/spiffs/log_backup.txt"

void init_spiffs(void);
int custom_log_write(const char* format, va_list args);
void save_spiffs_on_off_to_nvs(char *spiffs_on_off);
void remove_log_file(void);
unsigned long get_log_file_size(void);
unsigned long get_log_file_backup_size(void);
extern char spiffs_on_off_str[5];
#endif // SPIFFS_H