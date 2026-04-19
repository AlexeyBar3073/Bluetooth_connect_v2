// -----------------------------------------------------------------------------
// storage_task.cpp
// Хранение данных в NVS (Preferences).
//
// Назначение:
// - При старте: загрузка TripPack и SettingsPack из NVS → publish в DataRouter
// - В работе: подписка на TripPack и SettingsPack → сохранение в NVS
// - Бинарное хранение (putBytes/getBytes)
//
// Архитектура v5.0.0: Queue-архитектура, бинарные пакеты
//
// Обновление (интеграция с task_common):
// - Использует taskInit() для инициализации и подписки на TOPIC_CMD
// - Регистрирует подписки через taskRegisterSubscription()
// - taskProcessCommands() обрабатывает CMD_OTA_START с полной очисткой ресурсов
// - taskHeartbeat() обновляет счётчик для loop()-мониторинга
// - При OTA все очереди удаляются, подписки снимаются, задача завершается
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include "storage_task.h"
#include "data_router.h"
#include "task_common.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"
#include "debug.h"
#include <Preferences.h>
#include <math.h>

// =============================================================================
// Глобальные переменные
// =============================================================================

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;
static Preferences   prefs;

// --- Контекст задачи (фреймворк task_common) ---
// Внедрение фреймворка task_common:
//   - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
//   - Единый heartbeat для мониторинга в loop()
//   - Регистрация подписок для корректной отписки при завершении
static TaskContext ctx = {0};

// --- Очереди для подписки на изменения данных ---
// Создаются модулем, регистрируются через taskRegisterSubscription()
static QueueHandle_t tripQ      = NULL;
static QueueHandle_t settingsQ  = NULL;

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
// loadAndPublish — загрузка из NVS и публикация
// =============================================================================
// Вызывается ОДИН раз при старте системы, до запуска задачи хранения.
// Загружает сохранённые пакеты из NVS и публикует их в DataRouter.
// Если данных нет — создаёт значения по умолчанию.
//
// ВАЖНО: эта функция вызывается из storageStart() ДО создания задачи,
// чтобы другие модули получили начальные данные через retain-механизм.
//
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
        DBG_PRINTLN("[Storage] TripPack: defaults");
    } else {
        DBG_PRINTF("[Storage]   ODO=%.0f, tripA=%.1f, tripB=%.1f, fuel=%.1f, avg_total=%.1f\n",
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
        DBG_PRINTLN("[Storage] SettingsPack: defaults");
    }
    savedSettings = settingsPack;
    settingsValid = true;

    prefs.end();

    // --- Публикация начальных данных через DataRouter (кэш будет установлен) ---
    dr.publishPacket(TOPIC_TRIP_PACK, &tripPack, sizeof(tripPack));
    dr.publishPacket(TOPIC_SETTINGS_PACK, &settingsPack, sizeof(settingsPack));

    return true;
}

// =============================================================================
// storageCmdHandler — обработка специфичных команд модуля Storage
// =============================================================================
// Вызывается из taskProcessCommands() для каждой полученной команды.
// CMD_OTA_START обрабатывается автоматически во фреймворке, сюда не попадает.
//
// Storage не имеет специфичных команд, но обработчик необходим для
// корректной работы фреймворка. При появлении команд (например, CMD_FORCE_SAVE)
// они будут добавлены сюда.
//
// Параметры:
//   cmd — код команды (enum Command)
//
// Возвращает:
//   true  — команда обработана
//   false — команда не распознана (будет залогировано фреймворком)
//
static bool storageCmdHandler(uint8_t cmd) {
    switch ((Command)cmd) {
        case CMD_OTA_START:
            return true;  // ← ДОЛЖНО БЫТЬ TRUE
        default:
            return false;
    }
}

// =============================================================================
// storageTask — Задача сохранения в NVS
// =============================================================================
//
// Архитектура на основе task_common:
//   1. taskInit() — инициализация, создание cmdQ, подписка на TOPIC_CMD
//   2. Создание очередей и подписка на TOPIC_TRIP_PACK и TOPIC_SETTINGS_PACK
//   3. Основной цикл:
//      - taskHeartbeat() — обновление счётчика активности
//      - taskProcessCommands() — обработка команд (включая OTA)
//      - Сохранение изменённых пакетов в NVS с троттлингом
//
// При получении CMD_OTA_START:
//   - taskProcessCommands() вызывает taskShutdown()
//   - taskShutdown() отписывается от всех топиков, удаляет очереди, завершает задачу
//   - Память полностью освобождается для OTA-обновления
//
void storageTask(void* parameter) {
    (void)parameter;
    
    // === ИНИЦИАЛИЗАЦИЯ ЧЕРЕЗ ФРЕЙМВОРК ===
    // taskInit() выполняет:
    //   - Установку isRunningFlag = true
    //   - Создание cmdQ (очередь команд)
    //   - Подписку на TOPIC_CMD
    //   - Регистрацию подписки для автоматической очистки при shutdown
    if (!taskInit(&ctx, "Storage", &isRunningFlag, &lastHeartbeat)) {
        DBG_PRINTLN("[Storage] ERROR: taskInit failed!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }
    
    DataRouter& dr = DataRouter::getInstance();

    // --- Подписка на изменения для сохранения ---
    // Очереди создаются модулем и регистрируются в фреймворке для
    // автоматической очистки при завершении задачи.
    tripQ      = xQueueCreate(1, sizeof(TripPack));
    settingsQ  = xQueueCreate(1, sizeof(SettingsPack));

    if (!tripQ || !settingsQ) {
        DBG_PRINTLN("[Storage] ERROR: Failed to create queues!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }

    dr.subscribe(TOPIC_TRIP_PACK,     tripQ,     QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_SETTINGS_PACK, settingsQ, QueuePolicy::OVERWRITE);
    
    // Регистрируем подписки для автоматической отписки при shutdown
    taskRegisterSubscription(&ctx, TOPIC_TRIP_PACK, tripQ);
    taskRegisterSubscription(&ctx, TOPIC_SETTINGS_PACK, settingsQ);

    while (1) {
        // Heartbeat — обновление счётчика для loop()-мониторинга
        taskHeartbeat(&ctx);
        
        // Обработка команд (CMD_OTA_START обрабатывается автоматически)
        // При получении CMD_OTA_START эта функция НЕ ВОЗВРАЩАЕТСЯ
        taskProcessCommands(&ctx, storageCmdHandler);
        
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
                }
            }
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

// storageStart — запуск задачи хранения
// Вызывается из main.cpp в setup()
void storageStart() {
    if (!taskHandle) {
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunningFlag = true;
        
        // Сначала загружаем и публикуем — кэш будет установлен ДО подписки других модулей
        loadAndPublish();
        
        // Потом запускаем задачу сохранения
        // Ядро 1 — Storage (NVS сохранение)
        xTaskCreatePinnedToCore(storageTask, "Storage", TASK_STACK_SIZE, NULL, 1, &taskHandle, 1);
        DBG_PRINTLN("[Storage] Started (Core 1, task_common framework)");
    }
}

// storageStop — остановка задачи хранения
// Вызывается при необходимости принудительно завершить задачу
void storageStop() {
    if (taskHandle) {
        // Принудительное удаление задачи
        // ВНИМАНИЕ: ресурсы НЕ очищаются! Используйте только если задача зависла.
        // Для корректного завершения используйте CMD_OTA_START через шину.
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        DBG_PRINTLN("[Storage] Stopped (forced)");
    }
}

// storageIsRunning — проверка активности задачи
// Вызывается из loop() для мониторинга и перезапуска при зависании
bool storageIsRunning() { 
    return isRunningFlag && (millis() - lastHeartbeat) < 5000; 
}

// =============================================================================
// storageForceSave — Аварийное сохранение (вызывается при пропадании ACC)
// =============================================================================
//
// Назначение: мгновенная запись в NVS при выключении зажигания,
// пока ESP32 питается от конденсатора.
//
void storageForceSave() {
    DBG_PRINTLN("[Storage] === EMERGENCY SAVE ===");

    // Сохраняем TripPack (без throttle!)
    if (tripValid) {
        prefs.begin(NS, false);
        prefs.putBytes(KEY_TRIP, &savedTrip, sizeof(TripPack));
        prefs.end();
        DBG_PRINTF("[Storage] TripPack EMERGENCY saved: ODO=%.0f\n", savedTrip.odo);
    }

    // Сохраняем SettingsPack (без throttle!)
    if (settingsValid) {
        prefs.begin(NS, false);
        prefs.putBytes(KEY_SETTINGS, &savedSettings, sizeof(SettingsPack));
        prefs.end();
        DBG_PRINTLN("[Storage] SettingsPack EMERGENCY saved");
    }

    DBG_PRINTLN("[Storage] === EMERGENCY SAVE COMPLETE ===");
}