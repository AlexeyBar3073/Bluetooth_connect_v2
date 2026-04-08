// -----------------------------------------------------------------------------
// storage_task.cpp
// Хранение данных в NVS (Preferences).
//
// Назначение:
// - При старте: загрузка TripPack и SettingsPack из NVS → publish в DataBus
// - В работе: подписка на TripPack и SettingsPack → сохранение в NVS
// - Бинарное хранение (putBytes/getBytes)
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, бинарные пакеты
// -----------------------------------------------------------------------------

#include "storage_task.h"
#include "data_bus.h"
#include "topics.h"
#include "packets.h"
#include <Preferences.h>

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;
static Preferences   prefs;

// Throttle для NVS (не чаще 10 секунд)
static unsigned long lastTripSaveTime = 0;
static unsigned long lastSettingsSaveTime = 0;
#define NVS_THROTTLE_MS  10000

#define NS   "bkc_v2"
#define KEY_TRIP     "trip"
#define KEY_SETTINGS "settings"

// =============================================================================
// storageTask — Главная задача
// =============================================================================

void storageTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataBus& db = DataBus::getInstance();

    prefs.begin(NS, false);

    // --- Загрузка TripPack ---
    TripPack tripPack;
    memset(&tripPack, 0, sizeof(tripPack));
    size_t len = prefs.getBytes(KEY_TRIP, &tripPack, sizeof(tripPack));
    if (len != sizeof(TripPack) || tripPack.version != 1) {
        tripPack.version = 1;
        tripPack.fuel_level = 60.0f;
        Serial.println("[Storage] TripPack: defaults");
    } else {
        Serial.println("[Storage] TripPack: loaded");
    }

    // --- Загрузка SettingsPack ---
    SettingsPack settingsPack;
    memset(&settingsPack, 0, sizeof(settingsPack));
    len = prefs.getBytes(KEY_SETTINGS, &settingsPack, sizeof(settingsPack));
    if (len != sizeof(SettingsPack) || settingsPack.version != 1) {
        settingsPack.version = 1;
        settingsPack.tank_capacity = 60.0f;
        settingsPack.injector_count = 4;
        settingsPack.injector_flow = 250.0f;
        settingsPack.pulses_per_meter = 3.0f;
        settingsPack.kline_protocol = 0;
        Serial.println("[Storage] SettingsPack: defaults");
    } else {
        Serial.println("[Storage] SettingsPack: loaded");
    }

    prefs.end();

    // --- Публикация начальных данных (retain) ---
    SubscriberOpts retainOpts = {QUEUE_OVERWRITE, 1, false};
    db.subscribe(TOPIC_TRIP_PACK, retainOpts);       // Self-subscribe чтобы кэш заполнился
    db.subscribe(TOPIC_SETTINGS_PACK, retainOpts);

    db.publishPacket(TOPIC_TRIP_PACK, &tripPack, sizeof(tripPack));
    db.publishPacket(TOPIC_SETTINGS_PACK, &settingsPack, sizeof(settingsPack));
    Serial.println("[Storage] Initial data published");

    // --- Подписка на изменения для сохранения ---
    QueueHandle_t tripQ = db.subscribe(TOPIC_TRIP_PACK, retainOpts);
    QueueHandle_t settingsQ = db.subscribe(TOPIC_SETTINGS_PACK, retainOpts);

    Serial.println("[Storage] Task running (Queue-based, Binary NVS)");

    while (1) {
        lastHeartbeat = millis();

        // Сохранение TripPack при изменении (throttle 10 сек)
        BusMessage msg;
        unsigned long now = millis();
        if (xQueueReceive(tripQ, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
            if (now - lastTripSaveTime >= NVS_THROTTLE_MS) {
                lastTripSaveTime = now;
                TripPack p;
                memcpy(&p, msg.value.s, sizeof(TripPack));
                prefs.begin(NS, false);
                prefs.putBytes(KEY_TRIP, &p, sizeof(TripPack));
                prefs.end();
                Serial.printf("[Storage] TripPack saved: ODO=%.0f\n", p.odo);
            }
        }

        // Сохранение SettingsPack при изменении (throttle 10 сек)
        if (xQueueReceive(settingsQ, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
            if (now - lastSettingsSaveTime >= NVS_THROTTLE_MS) {
                lastSettingsSaveTime = now;
                SettingsPack p;
                memcpy(&p, msg.value.s, sizeof(SettingsPack));
                prefs.begin(NS, false);
                prefs.putBytes(KEY_SETTINGS, &p, sizeof(SettingsPack));
                prefs.end();
                Serial.printf("[Storage] SettingsPack saved\n");
            }
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void storageStart() {
    if (!taskHandle) {
        xTaskCreatePinnedToCore(storageTask, "Storage", 4096, NULL, 1, &taskHandle, 0);
        Serial.println("[Storage] Started");
    }
}

void storageStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        Serial.println("[Storage] Stopped");
    }
}

bool storageIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 5000; }
void storageForceSave() {}
