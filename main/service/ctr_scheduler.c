#include "ctr_scheduler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_log.h"
#include "uart_ctr.h"
#include "system_runtime.h"


#define TAG "CTR_SCHEDULER"

/* ====== 调度参数 ====== */
#define CTRL_GUARD_TIME_MS   300
#define READ_ALL_PERIOD_MS  200

/* ====== 调度状态 ====== */
static char     g_pending_read_all_cmd[64];
static bool     g_read_all_pending = false;

static uint32_t last_ctrl_tx_tick = 0;
static uint32_t last_read_all_tick = 0;

/* ====== 初始化 ====== */
void ctr_scheduler_init(void)
{
    g_pending_read_all_cmd[0] = 0;
    g_read_all_pending = false;
    last_ctrl_tx_tick = 0;
    last_read_all_tick = 0;
}

/* ====== 保护时间窗口判断 ====== */
static bool ctr_in_ctrl_guard_window(void)
{
    uint32_t now = xTaskGetTickCount();
    return (now - last_ctrl_tx_tick) < pdMS_TO_TICKS(CTRL_GUARD_TIME_MS);
}

/* ====== 调度参数 ====== */
void ctr_send_cmd(ctr_cmd_type_t type, const char *cmd)
{
    if (!cmd || cmd[0] == 0) return;

    if (sys_pra.app_mode == APP_MODE_YMODEM) {
        if (type == CTR_CMD_READ_ALL) {
            g_read_all_pending = false;
            g_pending_read_all_cmd[0] = 0;
        }
        ESP_LOGW(TAG, "Skip CTR command while APP_MODE_YMODEM, type=%d", (int)type);
        return;
    }

    if (type == CTR_CMD_CTRL) {
        /* 控制命令立即发送，并启动保护时间 */
        ctr_uart_send_data(cmd, strlen(cmd));
        last_ctrl_tx_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "CTR_CMD_CTRL.....");
    }
    else if (type == CTR_CMD_READ_ALL) {
        /* READ@ALL 指令进入延迟发送队列 */
        strncpy(g_pending_read_all_cmd, cmd,
                sizeof(g_pending_read_all_cmd) - 1);
        g_pending_read_all_cmd[sizeof(g_pending_read_all_cmd) - 1] = 0;
        g_read_all_pending = true;
    }
}

/* ====== 调度参数 ====== */
void ctr_scheduler_poll(void)
{
    uint32_t now = xTaskGetTickCount();

    if (sys_pra.app_mode == APP_MODE_YMODEM) {
        g_read_all_pending = false;
        g_pending_read_all_cmd[0] = 0;
        return;
    }

    /* 控制命令保护窗口内不发送轮询命令 */
    if (ctr_in_ctrl_guard_window()) {
        return;
    }

    /* 到达周期后发送一次 READ@ALL */
    if (g_read_all_pending) {
        if (now - last_read_all_tick < pdMS_TO_TICKS(READ_ALL_PERIOD_MS)) {
            return;
        }

        ctr_uart_send_data(g_pending_read_all_cmd,
                           strlen(g_pending_read_all_cmd));
        // ESP_LOGI(TAG, "CTR_CMD_READ_ALL.....");

        g_read_all_pending = false;
        last_read_all_tick = now;
    }
}



