#include "opus_player.h"
#include "sp_pro_app.h"
#include "mqtt_protocol.h"
#include "ram_diag.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "voice_mgr";

#define VOICE_QUEUE_LEN 4
#define AUDIO_TASK_STACK_DEFAULT 4096
#define AUDIO_TASK_STACK_TEST    3072
#define AUDIO_TASK_STACK         AUDIO_TASK_STACK_TEST
#define AUDIO_TASK_PRIO 5
#define VOICE_VOLUME_DEFAULT_LEVEL 5
#define VOICE_VOLUME_MAX_LEVEL 5
#define VOICE_VOLUME_PERCENT_STEP 20
#define VOICE_MIN_INTERNAL_FREE_BYTES 4096
#define VOICE_MIN_LARGEST_BLOCK_BYTES 2048

typedef struct {
    voice_id_t id;
    char filename[64];
    uint8_t priority;
} voice_req_t;

typedef struct {
    bool busy;
    voice_id_t current_id;
    uint8_t current_priority;
    voice_id_t followup_id;
    uint32_t last_end_tick[VOICE_MAX];
} voice_mgr_t;

typedef struct {
    const char *filename;
    uint8_t priority;
} voice_item_t;

typedef struct {
    bool configured;
    bool interruptible;
    bool barrier;
    uint32_t dedupe_ms;
} voice_policy_t;

static voice_mgr_t g_voice;
static QueueHandle_t voice_queue = NULL;
static TaskHandle_t audio_task_handle = NULL;
static const opus_player_t *player = NULL;
static bool audio_initialized = false;
static bool voice_req_pending = false;
static bool s_touch_tone_pending = false;

static void voice_manager_abort_current(void);

static const voice_item_t voice_table[VOICE_MAX] = {
    [VOICE_NONE] = { NULL, 0 },
    [VOICE_PRSCHILDLOCK3S2UNLOCK] = { "PrsChildLock3s2Unlock.opus", 3 },
    [VOICE_CAUTIONHOTSTEAM] = { "CautionHotSteam.opus", 3 },
    [VOICE_CANCELMAKECOFFEE] = { "CancelMakeCoffee.opus", 2 },
    [VOICE_ALERTBEANBINDISPLACED] = { "AlertBeanBinDisplaced.opus", 2 },
    [VOICE_KNOBATUNLOCKEDPOSITION] = { "KnobAtUnlockedPosition.opus", 2 },
    [VOICE_PORTAFILTERNOTPLACED] = { "PortafilterNotPlaced.opus", 2 },
    [VOICE_CLEANSTEAMWAND] = { "CleanSteamWand.opus", 2 },
    [VOICE_ADDWATERTOCLEANINGLINE] = { "AddWaterToCleaningLine.opus", 2 },
    [VOICE_ADDCLEANPOWDER] = { "AddCleanPowder.opus", 2 },
    [VOICE_SOAKSTEAMNOZZLE] = { "SoakSteamNozzle.opus", 2 },
    [VOICE_KEY] = { "key.opus", 1 },
    [VOICE_POWERON] = { "PowerOn.opus", 1 },
    [VOICE_POWEROFF] = { "PowerOff.opus", 1 },
    [VOICE_CLICKCLEANBUTTON] = { "ClickCleanButton.opus", 1 },
    [VOICE_WASHMILKPITCHER] = { "WashMilkPitcher.opus", 1 },
    [VOICE_SECONDSOAKSTEAMNOZZLE] = { "SecondSoakSteamNozzle.opus", 2 },
    [VOICE_CLEANFINISHED] = { "CleanFinished.opus", 2 },
    [VOICE_CONTINUECLEANAFTERPOWEROFF] = { "ContinueCleanAfterPowerOff.opus", 3 },
    [VOICE_CLEANFRONTBREWUNIT] = { "CleanFrontBrewUnit.opus", 2 },
    [VOICE_TANKMODEWATERADDREMINDER] = { "TankModeWaterAddReminder.opus", 2 },
    [VOICE_BUCKETMODEWATERADDREMINDER] = { "BucketModeWaterAddReminder.opus", 2 },
    [VOICE_PUTCLEANINGPOWDER] = { "PutCleaningPowder.opus", 2 },
    [VOICE_PROMPTHANDLEFITTING] = { "PromptHandleFitting.opus", 2 },
    [VOICE_TAKEOFFHANDLEANDWASH] = { "TakeOffHandleAndWash.opus", 2 },
    [VOICE_FINISHANDREMINDTODUMP] = { "FinishAndRemindToDump.opus", 2 },
    [VOICE_INSTALLHANDLE] = { "InstallHandle.opus", 2 },
    [VOICE_DESCALING] = { "Descaling.opus", 2 },
    [VOICE_SWITCHWATERTANKMODEREMIND] = { "SwitchWaterTankModeRemind.opus", 2 },
    [VOICE_ADDWATERANDDESCALINGPOWDER] = { "AddWaterAndDescalingPowder.opus", 2 },
    [VOICE_PROMPTPLACECONTAINER] = { "PromptPlaceContainer.opus", 2 },
    [VOICE_CHANGEWATER] = { "ChangeWater.opus", 2 },
    [VOICE_FINISHANDREMINDDUMPING] = { "FinishAndRemindDumping.opus", 2 },
    [VOICE_CURRENTWATERTANKMODE] = { "CurrentWaterTankMode.opus", 1 },
    [VOICE_CURRENTWATERBUCKETMODE] = { "CurrentWaterBucketMode.opus", 1 },
    [VOICE_WATERTANKMODE] = { "WaterTankMode.opus", 1 },
    [VOICE_WATERBUCKETMODE] = { "WaterBucketMode.opus", 1 },
    [VOICE_EXITSWITCHINLETMODE] = { "ExitSwitchInletMode.opus", 1 },
    [VOICE_SWITCHWATERHARDNESSLEVELA] = { "SwitchWaterHardnessLevelA.opus", 1 },
    [VOICE_SWITCHWATERHARDNESSLEVELB] = { "SwitchWaterHardnessLevelB.opus", 1 },
    [VOICE_SWITCHWATERHARDNESSLEVELC] = { "SwitchWaterHardnessLevelC.opus", 1 },
    [VOICE_WATERHARDNESSLEVELA] = { "WaterHardnessLevelA.opus", 1 },
    [VOICE_WATERHARDNESSLEVELB] = { "WaterHardnessLevelB.opus", 1 },
    [VOICE_WATERHARDNESSLEVELC] = { "WaterHardnessLevelC.opus", 1 },
    [VOICE_UPGRADEREMINDER] = { "UpgradeReminder.opus", 2 },
    [VOICE_UPGRADESUCCEEDED] = { "UpgradeSucceeded.opus", 2 },
    [VOICE_UPGRADEFAILED] = { "UpgradeFailed.opus", 3 },
    [VOICE_TANKMODEALERT] = { "TankModeAlert.opus", 2 },
    [VOICE_BUCKETMODEALERT] = { "BucketModeAlert.opus", 2 },
    [VOICE_EMPTYWATERTRAY] = { "EmptyWaterTray.opus", 2 },
    [VOICE_FACTORYRESTOREWARNING] = { "FactoryRestoreWarning.opus", 2 },
    [VOICE_AUTOPOWEROFF] = { "AutoPowerOff.opus", 1 },
    [VOICE_CONFIRMFACTORYRESET] = { "ConfirmFactoryReset.opus", 2 },
    [VOICE_FACTORYRESETCOMPLETED] = { "FactoryResetCompleted.opus", 2 },
    [VOICE_SYSTEMLACKSWATER] = { "SystemLacksWater.opus", 3 },
    [VOICE_WATERPUMPFAULT] = { "WaterPumpFault.opus", 3 },
    [VOICE_FIRSTFAULTWARNING] = { "FirstFaultWarning.opus", 2 },
    [VOICE_SECONDFAULTWARNING] = { "SecondFaultWarning.opus", 2 },
    [VOICE_LOWTEMPERATUREERROR] = { "LowTemperatureError.opus", 3 },
    [VOICE_PRESSURESENSORFAULTWARNING] = { "PressureSensorFaultWarning.opus", 3 },
    [VOICE_BEANHOPPERWARNING] = { "BeanHopperWarning.opus", 2 },
    [VOICE_PLACEGROUNDCUP] = { "PlaceGroundCup.opus", 1 },
    [VOICE_PLACECALIBRATIONWEIGHT] = { "PlaceCalibrationWeight.opus", 1 },
    [VOICE_CALIBRATIONCOMPLETED] = { "CalibrationCompleted.opus", 2 },
    [VOICE_HOTDRINKCALIBRATION] = { "HotDrinkCalibration.opus", 1 },
    [VOICE_HOTWATERCALIBRATION] = { "HotWaterCalibration.opus", 1 },
    [VOICE_PLACEPORTAFILTERONSTAND] = { "PlacePortafilterOnStand.opus", 1 },
    [VOICE_REMOVEPORTAFILTER] = { "RemovePortafilter.opus", 1 },
    [VOICE_REMOVEWATERTANK] = { "RemoveWaterTank.opus", 1 },
    [VOICE_PUTBACKWATERTANK] = { "PutBackWaterTank.opus", 1 },
    [VOICE_REMOVEBEANHOPPER] = { "RemoveBeanHopper.opus", 1 },
    [VOICE_PUTBACKBEANHOPPER] = { "PutBackBeanHopper.opus", 1 },
    [VOICE_UNLOCKHOPPER] = { "UnlockHopper.opus", 1 },
    [VOICE_DETECTIONPASSED] = { "DetectionPassed.opus", 2 },
    [VOICE_DETECTIONABNORMAL] = { "DetectionAbnormal.opus", 3 },
    [VOICE_GRINDINGWITHOUTHANDLE] = { "GrindingWithoutHandle.opus", 1 },
    [VOICE_STEAMNOTREADY] = { "SteamNotReady.opus", 1 },
    [VOICE_FILLWATERTOMAX] = { "FillWaterToMax.opus", 1 },
    [VOICE_BEANHOPPERMISSING] = { "BeanHopperMissing.opus", 1 },
    [VOICE_FILLCOFFEEBEANS] = { "FillCoffeeBeans.opus", 1 },
    [VOICE_WEIGHTABNORMAL] = { "WeightAbnormal.opus", 2 },
    /* Combined prompt: play BeanHopperMissing then FillWaterToMax. */
    [VOICE_BEANHOPPERMISSING_FILLWATERTOMAX] = { NULL, 2 },
};

static const voice_policy_t voice_policy_table[VOICE_MAX] = {
    [VOICE_FACTORYRESTOREWARNING] = {
        .configured = true,
        .interruptible = false,
        .barrier = true,
        .dedupe_ms = 0,
    },
    [VOICE_CONFIRMFACTORYRESET] = {
        .configured = true,
        .interruptible = false,
        .barrier = true,
        .dedupe_ms = 0,
    },
    [VOICE_CALIBRATIONCOMPLETED] = {
        .configured = true,
        .interruptible = false,
        .barrier = true,
        .dedupe_ms = 0,
    },
    [VOICE_DETECTIONPASSED] = {
        .configured = true,
        .interruptible = false,
        .barrier = true,
        .dedupe_ms = 0,
    },
    [VOICE_DETECTIONABNORMAL] = {
        .configured = true,
        .interruptible = false,
        .barrier = true,
        .dedupe_ms = 0,
    },
    [VOICE_UPGRADESUCCEEDED] = {
        .configured = true,
        .interruptible = false,
        .barrier = true,
        .dedupe_ms = 0,
    },
    [VOICE_UPGRADEFAILED] = {
        .configured = true,
        .interruptible = false,
        .barrier = true,
        .dedupe_ms = 0,
    },
};

static uint32_t get_tick_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static const setting_info_t *voice_manager_get_setting(void)
{
    return mqtt_get_runtime_setting();
}

static bool voice_manager_prompt_enabled(void)
{
    const setting_info_t *setting = voice_manager_get_setting();
    return !setting || setting->voice_prompt != 0;
}

static bool voice_manager_touch_tone_enabled(void)
{
    const setting_info_t *setting = voice_manager_get_setting();
    return !setting || setting->voice_touch_tone != 0;
}

static int voice_manager_output_volume(void)
{
    const setting_info_t *setting = voice_manager_get_setting();
    int volume_level = setting ? setting->voice_volume : VOICE_VOLUME_DEFAULT_LEVEL;

    if (volume_level < 0) {
        volume_level = 0;
    } else if (volume_level > VOICE_VOLUME_MAX_LEVEL) {
        volume_level = VOICE_VOLUME_MAX_LEVEL;
    }

    return volume_level * VOICE_VOLUME_PERCENT_STEP;
}

static void voice_manager_sync_volume(void)
{
    if (player && player->set_volume) {
        player->set_volume(voice_manager_output_volume());
    }
}

static bool voice_id_is_valid(voice_id_t id)
{
    return id > VOICE_NONE && id < VOICE_MAX;
}

static bool voice_manager_has_memory_headroom(const char *reason)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (internal_free < VOICE_MIN_INTERNAL_FREE_BYTES ||
        largest_block < VOICE_MIN_LARGEST_BLOCK_BYTES) {
        ESP_LOGW(TAG,
                 "voice rejected low memory reason=%s internal_free=%u largest=%u",
                 reason ? reason : "unknown",
                 (unsigned)internal_free,
                 (unsigned)largest_block);
        return false;
    }

    return true;
}

static voice_policy_t voice_manager_get_policy(voice_id_t id)
{
    voice_policy_t policy = {
        .configured = true,
        .interruptible = true,
        .barrier = false,
        .dedupe_ms = 0,
    };

    if (!voice_id_is_valid(id)) {
        return policy;
    }

    if (voice_policy_table[id].configured) {
        policy = voice_policy_table[id];
    }

    return policy;
}

static void voice_manager_set_idle(void)
{
    g_voice.busy = false;
    g_voice.current_id = VOICE_NONE;
    g_voice.current_priority = 0;
    voice_req_pending = false;
}

static bool voice_manager_build_req(voice_id_t id, voice_req_t *req, uint8_t priority_override)
{
    if (!voice_id_is_valid(id) || !req) {
        return false;
    }

    const voice_item_t *item = &voice_table[id];
    if (!item->filename) {
        return false;
    }

    memset(req, 0, sizeof(*req));
    req->id = id;
    req->priority = priority_override > 0 ? priority_override : item->priority;
    strncpy(req->filename, item->filename, sizeof(req->filename) - 1);
    return true;
}

static bool voice_manager_enqueue_req(voice_id_t id, bool send_to_front, uint8_t priority_override)
{
    voice_req_t req;
    BaseType_t result;

    if (!audio_initialized || !voice_queue) {
        return false;
    }

    if (!voice_manager_build_req(id, &req, priority_override)) {
        return false;
    }

    result = send_to_front ? xQueueSendToFront(voice_queue, &req, 0)
                           : xQueueSend(voice_queue, &req, 0);
    if (result != pdTRUE) {
        return false;
    }

    voice_req_pending = true;
    g_voice.busy = true;
    g_voice.current_id = id;
    g_voice.current_priority = req.priority;
    return true;
}

static bool voice_manager_start_special(voice_id_t id, bool send_to_front, uint8_t priority_override)
{
    if (id != VOICE_BEANHOPPERMISSING_FILLWATERTOMAX) {
        return false;
    }

    g_voice.followup_id = VOICE_FILLWATERTOMAX;
    return voice_manager_enqueue_req(VOICE_BEANHOPPERMISSING, send_to_front, priority_override);
}

static bool voice_manager_start_request(voice_id_t id, bool send_to_front, uint8_t priority_override)
{
    if (id == VOICE_BEANHOPPERMISSING_FILLWATERTOMAX) {
        return voice_manager_start_special(id, send_to_front, priority_override);
    }

    return voice_manager_enqueue_req(id, send_to_front, priority_override);
}

static bool voice_manager_reclaim_touch_tone_for_interrupt(voice_id_t id)
{
    if (!voice_id_is_valid(id) || !g_voice.busy || !voice_req_pending) {
        return false;
    }

    if (g_voice.current_id != VOICE_KEY) {
        return false;
    }

    ESP_LOGI(TAG,
             "reclaim touch tone for interrupt voice current_id=%d next_id=%d",
             (int)g_voice.current_id,
             (int)id);
    voice_manager_abort_current();
    s_touch_tone_pending = false;
    return true;
}

static void voice_manager_abort_current(void)
{
    xQueueReset(voice_queue);

    if (player) {
        player->stop();
    }

    voice_manager_set_idle();
    g_voice.followup_id = VOICE_NONE;
}

static bool voice_manager_request(voice_id_t id, bool allow_interrupt)
{
    voice_policy_t next_policy;
    uint32_t now;
    bool reclaimed_touch_tone = false;

    if (!audio_initialized || !voice_queue) {
        return false;
    }

    if (!voice_manager_prompt_enabled()) {
        if (allow_interrupt) {
            voice_manager_abort_current();
            s_touch_tone_pending = false;
        }
        return false;
    }

    if (!voice_id_is_valid(id)) {
        return false;
    }

    next_policy = voice_manager_get_policy(id);
    now = get_tick_ms();
    if (next_policy.dedupe_ms > 0U &&
        now - g_voice.last_end_tick[id] < next_policy.dedupe_ms) {
        ESP_LOGI(TAG, "voice deduped id=%d within %lu ms", (int)id, (unsigned long)next_policy.dedupe_ms);
        return false;
    }

    if (!voice_req_pending && !g_voice.busy) {
        if (!voice_manager_has_memory_headroom("prompt")) {
            return false;
        }
        return voice_manager_start_request(id, false, 0);
    }

    if (!allow_interrupt) {
        return false;
    }

    if (g_voice.current_id == id) {
        ESP_LOGI(TAG, "voice interrupt ignored: same id=%d", (int)id);
        return false;
    }

    if (voice_id_is_valid(g_voice.current_id)) {
        voice_policy_t current_policy = voice_manager_get_policy(g_voice.current_id);
        if (current_policy.barrier || !current_policy.interruptible) {
            ESP_LOGI(TAG,
                     "voice interrupt blocked by barrier current_id=%d next_id=%d",
                     (int)g_voice.current_id,
                     (int)id);
            return false;
        }
    }

    if (!voice_manager_has_memory_headroom("prompt")) {
        reclaimed_touch_tone = voice_manager_reclaim_touch_tone_for_interrupt(id);
        if (!reclaimed_touch_tone ||
            !voice_manager_has_memory_headroom("prompt_after_reclaim")) {
            return false;
        }
    }

    if (!reclaimed_touch_tone) {
        voice_manager_abort_current();
    }
    return voice_manager_start_request(id, true, 0xFF);
}

static void on_play_finish(const char *filename)
{
    uint32_t now = get_tick_ms();
    voice_id_t finished_id = g_voice.current_id;
    voice_id_t followup_id = g_voice.followup_id;

    (void)filename;
    if (finished_id < VOICE_MAX) {
        g_voice.last_end_tick[finished_id] = now;
    }

    if (followup_id != VOICE_NONE) {
        g_voice.followup_id = VOICE_NONE;
        if (voice_manager_enqueue_req(followup_id, true, 0)) {
            ESP_LOGI(TAG, "play chained: %d -> %d", finished_id, followup_id);
            return;
        }
    }

    if (finished_id == VOICE_KEY && s_touch_tone_pending) {
        s_touch_tone_pending = false;
        if (voice_manager_enqueue_req(VOICE_KEY, true, 0)) {
            ESP_LOGI(TAG, "play chained touch tone");
            return;
        }
    }

    voice_manager_set_idle();
    ESP_LOGI(TAG, "play finished");
}

static void on_play_error(const char *filename, int error_code)
{
    s_touch_tone_pending = false;
    voice_manager_set_idle();
    ESP_LOGE(TAG,
             "play error file=%s code=%d current_id=%d followup_id=%d",
             filename ? filename : "(null)",
             error_code,
             (int)g_voice.current_id,
             (int)g_voice.followup_id);
}

static void audio_task(void *arg)
{
    voice_req_t req;
    UBaseType_t watermark;
    bool workload_logged = false;

    (void)arg;
    watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG,
             "audio_task start, stack=%u bytes, high_water=%u words",
             (unsigned)AUDIO_TASK_STACK,
             (unsigned)watermark);

    while (1) {
        if (xQueueReceive(voice_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!player) {
            voice_manager_set_idle();
            continue;
        }

        g_voice.busy = true;
        g_voice.current_id = req.id;
        g_voice.current_priority = req.priority;
        voice_manager_sync_volume();
        ESP_LOGI(TAG, "Play: %s", req.filename);
        if (!workload_logged) {
            workload_logged = true;
            watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG,
                     "audio_task first playback request, high_water=%u words",
                     (unsigned)watermark);
        }
        player->play(req.filename, true);
    }
}

void voice_manager_init(void)
{
    if (audio_initialized) {
        return;
    }

    ram_diag_snapshot("voice/init_start");
    memset(&g_voice, 0, sizeof(g_voice));
    g_voice.current_id = VOICE_NONE;
    g_voice.followup_id = VOICE_NONE;
    player = opus_player_get_instance();

    if (!player) {
        ESP_LOGE(TAG, "player null");
        return;
    }

    opus_player_config_t config = {
        .i2c_sda_io = 18,
        .i2c_scl_io = 17,
        .i2c_addr = 0x30,
        .i2s_mclk_io = 38,
        .i2s_bclk_io = 47,
        .i2s_ws_io = 21,
        .i2s_dout_io = 13,
        .i2s_din_io = 0,
        .pa_gpio = 14,
        .volume = voice_manager_output_volume(),
        .server_url = NULL,
        .on_play_start = NULL,
        .on_play_finish = on_play_finish,
        .on_play_error = on_play_error,
    };

    if (player->init_ex(&config) != ESP_OK) {
        ESP_LOGE(TAG, "player init fail");
        ram_diag_snapshot("voice/player_init_fail");
        return;
    }
    ram_diag_snapshot("voice/after_player_init");

    voice_queue = xQueueCreate(VOICE_QUEUE_LEN, sizeof(voice_req_t));
    if (!voice_queue) {
        ESP_LOGE(TAG, "voice queue create fail");
        ram_diag_snapshot("voice/queue_create_fail");
        return;
    }
    ram_diag_snapshot("voice/after_queue_create");

    if (xTaskCreate(audio_task,
                    "audio_task",
                    AUDIO_TASK_STACK,
                    NULL,
                    AUDIO_TASK_PRIO,
                    &audio_task_handle) != pdPASS) {
        ESP_LOGE(TAG,
                 "audio task create fail, stack=%u bytes",
                 (unsigned)AUDIO_TASK_STACK);
        vQueueDelete(voice_queue);
        voice_queue = NULL;
        ram_diag_snapshot("voice/task_create_fail");
        return;
    }
    ESP_LOGI(TAG,
             "audio task created, stack=%u bytes",
             (unsigned)AUDIO_TASK_STACK);

    audio_initialized = true;
    ESP_LOGI(TAG, "voice manager ready");
    ram_diag_snapshot("voice/init_done");
}

bool voice_manager_play(voice_id_t id)
{
    return voice_manager_request(id, false);
}

bool voice_manager_play_touch_tone(void)
{
    voice_req_t req;

    if (!audio_initialized || !voice_queue) {
        ESP_LOGW(TAG, "touch tone rejected: audio not ready");
        return false;
    }

    if (!voice_manager_touch_tone_enabled()) {
        return false;
    }

    if (!voice_manager_has_memory_headroom("touch_tone")) {
        return false;
    }

    if (voice_req_pending || g_voice.busy) {
        if (g_voice.current_id == VOICE_KEY) {
            s_touch_tone_pending = true;
            ESP_LOGI(TAG, "touch tone deferred behind current key");
            return true;
        }
        ESP_LOGI(TAG,
                 "touch tone skipped busy current_id=%d pending=%d",
                 (int)g_voice.current_id,
                 (int)voice_req_pending);
        return false;
    }

    if (!voice_manager_build_req(VOICE_KEY, &req, 0)) {
        ESP_LOGW(TAG, "touch tone rejected: build req failed");
        return false;
    }

    if (xQueueSend(voice_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "touch tone enqueue failed");
        return false;
    }

    voice_req_pending = true;
    g_voice.busy = true;
    g_voice.current_id = req.id;
    g_voice.current_priority = req.priority;
    return true;
}

bool voice_manager_play_interrupt(voice_id_t id)
{
    if (!audio_initialized || !voice_queue) {
        return false;
    }

    return voice_manager_request(id, true);
}

void voice_manager_stop(void)
{
    if (!audio_initialized || !voice_queue) {
        return;
    }

    xQueueReset(voice_queue);

    if (player) {
        player->stop();
    }

    voice_manager_set_idle();
    g_voice.followup_id = VOICE_NONE;
    s_touch_tone_pending = false;
}

bool voice_manager_interval(voice_id_t id, uint32_t interval_ms)
{
    uint32_t now = get_tick_ms();

    if (!audio_initialized || !voice_id_is_valid(id)) {
        return false;
    }

    if (!voice_manager_prompt_enabled()) {
        return false;
    }

    if (now - g_voice.last_end_tick[id] < interval_ms) {
        return false;
    }

    return voice_manager_play(id);
}

bool voice_play_is_busy(void)
{
    return g_voice.busy;
}

void voice_manager_clear_queue(void)
{
    if (!audio_initialized || !voice_queue) {
        return;
    }

    xQueueReset(voice_queue);
    voice_req_pending = g_voice.busy;
    s_touch_tone_pending = false;
}
