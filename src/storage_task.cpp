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
#include <math.h>

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;
static Preferences   prefs;

// Throttle для NVS
static unsigned long lastTripSaveTime = 0;
static unsigned long lastSettingsSaveTime = 0;
#define TRIP_THROTTLE_MS  60000  // 60 сек — odo меняется медленно
#define SETTINGS_THROTTLE_MS  2000  // 2 сек — настройки меняются редко

// Копии сохранённых пакетов для сравнения
static TripPack      savedTrip = {0};
static SettingsPack  savedSettings = {0};
static bool          tripValid = false;
static bool          settingsValid = false;

#define NS   "bkc_v2"
#define KEY_TRIP     "trip"
#define KEY_SETTINGS "settings"

// =============================================================================
// Сравнение пакетов по полям (с допуском для float)
// =============================================================================

static bool tripPackChanged(const TripPack& a, const TripPack& b) {
    // Сравниваем ТОЛЬКО base-значения — то, что реально сохраняется в NVS.
    // trip_cur/fuel_cur меняются постоянно — их не сохраняем.
    const float F_EPS = 0.1f;  // Увеличенный допуск для fuel
    if (a.version != b.version) return true;
    if (fabs((double)a.odo - (double)b.odo) > F_EPS) return true;
    if (fabsf(a.trip_a - b.trip_a) > F_EPS) return true;
    if (fabsf(a.trip_b - b.trip_b) > F_EPS) return true;
    if (fabsf(a.fuel_trip_a - b.fuel_trip_a) > F_EPS) return true;
    if (fabsf(a.fuel_trip_b - b.fuel_trip_b) > F_EPS) return true;
    if (fabsf(a.fuel_level - b.fuel_level) > F_EPS) return true;
    // trip_cur, fuel_cur, avg_consumption — не сохраняем, пересчитываются
    return false;
}

static bool settingsPackChanged(const SettingsPack& a, const SettingsPack& b) {
    const float F_EPS = 0.01f;
    if (a.version != b.version) return true;
    if (fabsf(a.tank_capacity - b.tank_capacity) > F_EPS) return true;
    if (a.injector_count != b.injector_count) return true;
    if (fabsf(a.injector_flow - b.injector_flow) > F_EPS) return true;
    if (fabsf(a.pulses_per_meter - b.pulses_per_meter) > F_EPS) return true;
    if (a.kline_protocol != b.kline_protocol) return true;
    return false;
}

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
    // Сохраняем копию для сравнения
    savedTrip = tripPack;
    tripValid = true;

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
    // Сохраняем копию для сравнения
    savedSettings = settingsPack;
    settingsValid = true;

    prefs.end();

    // --- Публикация начальных данных ---
    db.publishPacket(TOPIC_TRIP_PACK, &tripPack, sizeof(tripPack));
    db.publishPacket(TOPIC_SETTINGS_PACK, &settingsPack, sizeof(settingsPack));
    Serial.println("[Storage] Initial data published");

    // --- Подписка на изменения для сохранения ---
    SubscriberOpts saveOpts = {QUEUE_OVERWRITE, 1, false};
    QueueHandle_t tripQ = db.subscribe(TOPIC_TRIP_PACK, saveOpts);
    QueueHandle_t settingsQ = db.subscribe(TOPIC_SETTINGS_PACK, saveOpts);

    Serial.println("[Storage] Task running (Queue-based, Binary NVS)");

    while (1) {
        lastHeartbeat = millis();

        // Сохранение TripPack при изменении (throttle 60 сек)
        BusMessage msg;
        unsigned long now = millis();
        if (xQueueReceive(tripQ, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
            TripPack p;
            memcpy(&p, msg.value.s, sizeof(TripPack));

            // Сохраняем только если данные реально изменились
            if (now - lastTripSaveTime >= TRIP_THROTTLE_MS) {
                if (!tripValid || tripPackChanged(p, savedTrip)) {
                    lastTripSaveTime = now;
                    savedTrip = p;
                    tripValid = true;
                    prefs.begin(NS, false);
                    prefs.putBytes(KEY_TRIP, &p, sizeof(TripPack));
                    prefs.end();
                    Serial.printf("[Storage] TripPack saved: ODO=%.0f\n", p.odo);
                }
            }
            busMessageFree(&msg);
        }

        // Сохранение SettingsPack при изменении (throttle 10 сек)
        if (xQueueReceive(settingsQ, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
            SettingsPack p;
            memcpy(&p, msg.value.s, sizeof(SettingsPack));

            // Сохраняем только если данные реально изменились
            if (now - lastSettingsSaveTime >= SETTINGS_THROTTLE_MS) {
                if (!settingsValid || settingsPackChanged(p, savedSettings)) {
                    lastSettingsSaveTime = now;
                    savedSettings = p;
                    settingsValid = true;
                    prefs.begin(NS, false);
                    prefs.putBytes(KEY_SETTINGS, &p, sizeof(SettingsPack));
                    prefs.end();
                    Serial.println("[Storage] SettingsPack saved");
                }
            }
            busMessageFree(&msg);
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



