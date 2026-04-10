// -----------------------------------------------------------------------------
// storage_task.cpp
// Хранение данных в NVS (Preferences).
//
// Назначение:
// - При старте: загрузка TripPack и SettingsPack из NVS → publish в DataRouter
// - В работе: подписка на TripPack и SettingsPack → сохранение в NVS
// - Бинарное хранение (putBytes/getBytes)
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, бинарные пакеты
// -----------------------------------------------------------------------------

#include "storage_task.h"
#include "data_router.h"
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
    const float F_EPS = 0.1f;
    if (a.version != b.version) return true;
    if (fabs((double)a.odo - (double)b.odo) > F_EPS) return true;
    if (fabsf(a.trip_a - b.trip_a) > F_EPS) return true;
    if (fabsf(a.trip_b - b.trip_b) > F_EPS) return true;
    if (fabsf(a.fuel_trip_a - b.fuel_trip_a) > F_EPS) return true;
    if (fabsf(a.fuel_trip_b - b.fuel_trip_b) > F_EPS) return true;
    if (fabsf(a.fuel_level - b.fuel_level) > F_EPS) return true;
    if (fabsf(a.avg_total - b.avg_total) > F_EPS) return true;
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

// =============================================================================
// loadAndPublish — загрузка из NVS и публикация
// =============================================================================

static bool loadAndPublish() {
    DataRouter& dr = DataRouter::getInstance();

    prefs.begin(NS, false);

    // --- Загрузка TripPack ---
    TripPack tripPack;
    memset(&tripPack, 0, sizeof(tripPack));
    size_t len = prefs.getBytes(KEY_TRIP, &tripPack, sizeof(tripPack));
    if (len != sizeof(TripPack) || tripPack.version < 1 || tripPack.version > 2) {
        tripPack.version = 2;
        tripPack.fuel_level = 60.0f;
        Serial.println("[Storage] TripPack: defaults");
    } else {
        Serial.println("[Storage] TripPack: loaded");
        Serial.printf("[Storage]   ODO=%.0f, tripA=%.1f, tripB=%.1f, fuel=%.1f, avg_total=%.1f\n",
                      tripPack.odo, tripPack.trip_a, tripPack.trip_b, tripPack.fuel_level, tripPack.avg_total);
    }
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
    savedSettings = settingsPack;
    settingsValid = true;

    prefs.end();

    // --- Публикация начальных данных через DataRouter (кэш будет установлен) ---
    dr.publishPacket(TOPIC_TRIP_PACK, &tripPack, sizeof(tripPack));
    dr.publishPacket(TOPIC_SETTINGS_PACK, &settingsPack, sizeof(settingsPack));
    Serial.println("[Storage] Initial data published (DataRouter)");

    return true;
}

// =============================================================================
// storageTask — Задача сохранения в NVS
// =============================================================================

void storageTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;

    // --- Подписка на изменения для сохранения ---
    QueueHandle_t tripQ      = xQueueCreate(1, sizeof(TripPack));
    QueueHandle_t settingsQ  = xQueueCreate(1, sizeof(SettingsPack));

    DataRouter& dr = DataRouter::getInstance();
    dr.subscribe(TOPIC_TRIP_PACK,     tripQ,     QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_SETTINGS_PACK, settingsQ, QueuePolicy::OVERWRITE);

    Serial.println("[Storage] Task running (DataRouter, Binary NVS)");

    while (1) {
        lastHeartbeat = millis();
        unsigned long now = millis();

        // Сохранение TripPack при изменении (throttle 60 сек)
        TripPack p;
        if (xQueueReceive(tripQ, &p, 0) == pdTRUE) {
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
        }

        // Сохранение SettingsPack при изменении (throttle 2 сек)
        SettingsPack sp;
        if (xQueueReceive(settingsQ, &sp, 0) == pdTRUE) {
            if (now - lastSettingsSaveTime >= SETTINGS_THROTTLE_MS) {
                if (!settingsValid || settingsPackChanged(sp, savedSettings)) {
                    lastSettingsSaveTime = now;
                    savedSettings = sp;
                    settingsValid = true;
                    prefs.begin(NS, false);
                    prefs.putBytes(KEY_SETTINGS, &sp, sizeof(SettingsPack));
                    prefs.end();
                    Serial.println("[Storage] SettingsPack saved");
                }
            }
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void storageStart() {
    if (!taskHandle) {
        // Сначала загружаем и публикуем — кэш будет установлен ДО подписки других модулей
        loadAndPublish();
        // Потом запускаем задачу сохранения
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



