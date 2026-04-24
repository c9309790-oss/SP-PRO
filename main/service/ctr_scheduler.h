#ifndef __CTR_SCHEDULER_H__
#define __CTR_SCHEDULER_H__

#include <stdint.h>
#include <stdbool.h>

/* ===== CTR 命令类型 ===== */
typedef enum {
    CTR_CMD_CTRL = 0,     // 控制类命令，优先立即发送
    CTR_CMD_READ_ALL      // 状态轮询类命令
} ctr_cmd_type_t;

/* ===== 初始化 ===== */
void ctr_scheduler_init(void);

/* ===== 提交一条 CTR 命令 ===== */
void ctr_send_cmd(ctr_cmd_type_t type, const char *cmd);

/* ===== 周期轮询，处理延迟命令 ===== */
void ctr_scheduler_poll(void);

#endif /* __CTR_SCHEDULER_H__ */



