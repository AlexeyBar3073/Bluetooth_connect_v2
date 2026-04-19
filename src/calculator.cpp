// -----------------------------------------------------------------------------
// calculator.cpp
// Вычислитель — расчётное ядро бортового компьютера.
//
// Назначение:
// - Подписан на TOPIC_ENGINE_PACK (EnginePack) → получает distance, fuel_used, eng
// - Подписан на TOPIC_TRIP_PACK (TripPack, retain) → получает base-значения от Storage
// - Подписан на TOPIC_SETTINGS_PACK (SettingsPack, retain) → получает tank_capacity
// - Подписан на TOPIC_NOT_FUEL (bool, retain) → режим расчёта топлива
// - Подписан на TOPIC_CMD (FIFO_DROP, depth=5) → команды управления
// - Рассчитывает TripPack: odo = base + distance, trip = base + distance, и т.д.
// - Публикует TripPack в TOPIC_TRIP_PACK каждые 1000 мс
//
// Формулы:
//   odo_current     = odo_base + current_distance
//   trip_a_current  = trip_a_base + current_distance
//   fuel_a_current  = fuel_trip_a_base + current_fuel_used
//   avg_consumption = (current_fuel_used / current_distance) * 100.0
//   fuel_level      = fuel_base - current_fuel_used (если not_fuel)
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ можно:
//   - Добавлять новые формулы расчёта
//   - Добавлять обработку новых команд
//   - Менять частоту публикации TripPack
//
// ❌ нельзя:
//   - Менять base-значения без явной команды (reset_*, correct_odo, full_tank)
//   - Писать в NVS напрямую
//   - Вызывать другие модули напрямую
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

#include "calculator.h"
#include "data_router.h"
#include "task_common.h"
#include "app_config.h"
#include "debug.h"

// =============================================================================
// Глобальные переменные
// =============================================================================

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;

// --- Контекст задачи (фреймворк task_common) ---
// Внедрение фреймворка task_common:
//   - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
//   - Единый heartbeat для мониторинга в loop()
//   - Регистрация подписок для корректной отписки при завершении
static TaskContext ctx = {0};

// --- Очереди для подписки на данные ---
// Создаются модулем, регистрируются через taskRegisterSubscription()
static QueueHandle_t engineQ     = NULL;
static QueueHandle_t tripQ       = NULL;
static QueueHandle_t settingsQ   = NULL;
static QueueHandle_t correctOdoQ = NULL;

// --- Base-значения (от Storage, не меняются до команд) ---
static double  odo_base          = 0.0;
static float   trip_a_base       = 0.0f;
static float   trip_b_base       = 0.0f;
static float   fuel_trip_a_base  = 0.0f;
static float   fuel_trip_b_base  = 0.0f;
static float   fuel_base         = 60.0f;

// --- Инициализация от Storage ---
static bool    storageInit       = false;
static bool    fuelLoaded        = false;

// --- Накопленные за поездку ---
static float   current_distance  = 0.0f;
static float   current_fuel_used = 0.0f;

// --- Средний расход ---
static float   avg_cur           = 0.0f;   // За текущую поездку
static float   avg_total         = 0.0f;   // Накопленный за всё время

// --- Топливо от датчика (EnginePack) ---
static float   fuel_level_sensor = 0.0f;  // Рассчитан EngineModule/Simulator

// --- Статусы ---
static bool    not_fuel          = false;  // Датчик топлива есть (Simulator считает)
static bool    engineRunning     = false;

// --- Настройки ---
static float   tank_capacity     = 60.0f;

// =============================================================================
// processEnginePack: Чтение EnginePack из очереди
// =============================================================================
static void processEnginePack(QueueHandle_t q) {
    EnginePack pack;
    if (xQueueReceive(q, &pack, 0) == pdTRUE) {
        if (pack.engine_running && !engineRunning) {
            current_distance  = 0.0f;
            current_fuel_used = 0.0f;
            avg_cur = 0.0f;
            DBG_PRINTLN("[Calculator] Engine started, trip counters reset");
        }
        if (!pack.engine_running && engineRunning) {
            // Двигатель заглушён — пересчитываем avg_total
            if (avg_total == 0.0f || avg_cur == 0.0f) {
                avg_total = avg_cur;
            } else {
                avg_total = (avg_total + avg_cur) / 2.0f;
            }
            DBG_PRINTF("[Calculator] Engine stopped, avg_total=%.1f", avg_total);
        }
        engineRunning = pack.engine_running;
        current_distance  = pack.distance;
        current_fuel_used = pack.fuel_used;
        fuel_level_sensor = pack.fuel_level_sensor;
        not_fuel = pack.not_fuel;
    }
}

// =============================================================================
// processTripPack: Чтение TripPack от Storage (base-значения)
// =============================================================================
static void processTripPack(QueueHandle_t q) {
    TripPack pack;
    if (xQueueReceive(q, &pack, 0) == pdTRUE) {
        if (odo_base == 0.0 && pack.odo > 0.01 && !engineRunning) {
            odo_base = pack.odo;
            trip_a_base = pack.trip_a;
            trip_b_base = pack.trip_b;
            fuel_trip_a_base = pack.fuel_trip_a;
            fuel_trip_b_base = pack.fuel_trip_b;
            // Загружаем avg_total из NVS (если есть)
            if (pack.avg_total > 0.0f) {
                avg_total = pack.avg_total;
            }
        }
        // fuel_level из первого TripPack (из NVS) — всегда берём
        if (!fuelLoaded && pack.fuel_level > 0.01f) {
            fuelLoaded = true;
            fuel_base = pack.fuel_level;
        }
    }
}

// =============================================================================
// processSettingsPack: Чтение настроек
// =============================================================================
static void processSettingsPack(QueueHandle_t q) {
    SettingsPack pack;
    if (xQueueReceive(q, &pack, 0) == pdTRUE) {
        if (!storageInit) {
            storageInit = true;
            DBG_PRINTF("[Calculator] SettingsPack from storage: tank=%.1f", pack.tank_capacity);
        }
        float oldTank = tank_capacity;
        tank_capacity = pack.tank_capacity;
        if (oldTank != tank_capacity && fuel_base > tank_capacity) {
            fuel_base = tank_capacity;
            DBG_PRINTF("[Calculator] Tank capacity changed: %.1f -> %.1f L, fuel_base corrected",
                      oldTank, tank_capacity);
        }
    }
}

// =============================================================================
// processCorrectOdo: Чтение нового значения ODO (int)
// =============================================================================
static void processCorrectOdo(QueueHandle_t q) {
    int newOdo;
    if (xQueueReceive(q, &newOdo, 0) == pdTRUE) {
        DBG_PRINTF("[Calculator] ODO corrected: %d km", newOdo);
        odo_base = (double)newOdo;
    }
}

// =============================================================================
// calcCmdHandler — обработка специфичных команд модуля Calculator
// =============================================================================
// Вызывается из taskProcessCommands() для каждой полученной команды.
// CMD_OTA_START обрабатывается автоматически во фреймворке, сюда не попадает.
//
// Параметры:
//   cmd — код команды (enum Command)
//
// Возвращает:
//   true  — команда обработана
//   false — команда не распознана (будет залогировано фреймворком)
//
static bool calcCmdHandler(uint8_t cmd) {
    switch ((Command)cmd) {
        case CMD_RESET_TRIP_A:
            trip_a_base = -current_distance;
            fuel_trip_a_base = -current_fuel_used;
            DBG_PRINTLN("[Calculator] Trip A reset");
            return true;

        case CMD_RESET_TRIP_B:
            trip_b_base = -current_distance;
            fuel_trip_b_base = -current_fuel_used;
            DBG_PRINTLN("[Calculator] Trip B reset");
            return true;

        case CMD_RESET_AVG:
            current_distance  = 0.0f;
            current_fuel_used = 0.0f;
            avg_cur = 0.0f;
            DBG_PRINTLN("[Calculator] Avg consumption reset");
            return true;

        case CMD_FULL_TANK:
            fuel_base = tank_capacity;
            current_fuel_used = 0.0f;
            DBG_PRINTF("[Calculator] Full tank: fuel_base = %.1f L", fuel_base);
            return true;

        case CMD_OTA_START:
            return true;  // Завершить задачу при OTA

        default:
            return false;  // Команда не распознана
    }
}

// =============================================================================
// calculatorTask — Главная задача FreeRTOS
// =============================================================================
//
// Архитектура на основе task_common:
//   1. taskInit() — инициализация, создание cmdQ, подписка на TOPIC_CMD
//   2. Создание очередей и подписка на топики данных
//   3. Основной цикл:
//      - taskHeartbeat() — обновление счётчика активности
//      - Чтение всех очередей данных
//      - taskProcessCommands() — обработка команд (включая OTA)
//      - Расчёт и публикация TripPack каждые 1000 мс
//
// При получении CMD_OTA_START:
//   - taskProcessCommands() вызывает taskShutdown()
//   - taskShutdown() отписывается от всех топиков, удаляет очереди, завершает задачу
//   - Память полностью освобождается для OTA-обновления
//
void calculatorTask(void* parameter) {
    (void)parameter;

    // === ИНИЦИАЛИЗАЦИЯ ЧЕРЕЗ ФРЕЙМВОРК ===
    // taskInit() выполняет:
    //   - Установку isRunningFlag = true
    //   - Создание cmdQ (очередь команд)
    //   - Подписку на TOPIC_CMD
    //   - Регистрацию подписки для автоматической очистки при shutdown
    if (!taskInit(&ctx, "Calculator", &isRunningFlag, &lastHeartbeat)) {
        DBG_PRINTLN("[Calculator] ERROR: taskInit failed!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }

    DataRouter& dr = DataRouter::getInstance();

    // === СОЗДАНИЕ ОЧЕРЕДЕЙ И ПОДПИСКА НА ТОПИКИ ===
    // Очереди создаются модулем и регистрируются в фреймворке для
    // автоматической очистки при завершении задачи.
    engineQ     = xQueueCreate(1, sizeof(EnginePack));
    tripQ       = xQueueCreate(1, sizeof(TripPack));
    settingsQ   = xQueueCreate(1, sizeof(SettingsPack));
    correctOdoQ = xQueueCreate(1, sizeof(int));

    if (!engineQ || !tripQ || !settingsQ || !correctOdoQ) {
        DBG_PRINTLN("[Calculator] ERROR: Failed to create queues!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }

    dr.subscribe(TOPIC_ENGINE_PACK,    engineQ,     QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRIP_PACK,      tripQ,       QueuePolicy::OVERWRITE, true);   // retain
    dr.subscribe(TOPIC_SETTINGS_PACK,  settingsQ,   QueuePolicy::OVERWRITE, true);   // retain
    dr.subscribe(TOPIC_CORRECT_ODO,    correctOdoQ, QueuePolicy::OVERWRITE);

    // Регистрируем подписки для автоматической отписки при shutdown
    taskRegisterSubscription(&ctx, TOPIC_ENGINE_PACK,   engineQ);
    taskRegisterSubscription(&ctx, TOPIC_TRIP_PACK,     tripQ);
    taskRegisterSubscription(&ctx, TOPIC_SETTINGS_PACK, settingsQ);
    taskRegisterSubscription(&ctx, TOPIC_CORRECT_ODO,   correctOdoQ);

    unsigned long lastPublish = 0;
    const unsigned long PUBLISH_INTERVAL = 1000;

    while (1) {
        // Heartbeat — обновление счётчика для loop()-мониторинга
        taskHeartbeat(&ctx);

        // Читаем все доступные сообщения из очередей
        if (engineQ)      processEnginePack(engineQ);
        if (tripQ)        processTripPack(tripQ);
        if (settingsQ)    processSettingsPack(settingsQ);
        if (correctOdoQ)  processCorrectOdo(correctOdoQ);
        
        // Обработка команд (CMD_OTA_START обрабатывается автоматически)
        // При получении CMD_OTA_START эта функция НЕ ВОЗВРАЩАЕТСЯ
        taskProcessCommands(&ctx, calcCmdHandler);

        // Расчёт и публикация TripPack каждую секунду
        unsigned long now = millis();
        if (now - lastPublish >= PUBLISH_INTERVAL) {
            lastPublish = now;

            TripPack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version         = 1;
            pack.odo             = odo_base + current_distance;
            pack.trip_a          = trip_a_base + current_distance;
            pack.fuel_trip_a     = fuel_trip_a_base + current_fuel_used;
            pack.trip_b          = trip_b_base + current_distance;
            pack.fuel_trip_b     = fuel_trip_b_base + current_fuel_used;
            pack.trip_cur        = current_distance;
            pack.fuel_cur        = current_fuel_used;

            // Топливо: от датчика ИЛИ расчёт
            if (not_fuel) {
                pack.fuel_level = max(0.0f, fuel_base - current_fuel_used);
            } else {
                pack.fuel_level = fuel_level_sensor;
            }

            pack.avg_consumption = (current_distance > 0.001f) ? (current_fuel_used / current_distance) * 100.0f : 0.0f;
            avg_cur = pack.avg_consumption;
            pack.avg_total = avg_total;

            dr.publishPacket(TOPIC_TRIP_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void calculatorStart() {
    if (!taskHandle) {
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunningFlag = true;
        // Ядро 1 — Calculator (TripPack, расход, одометр)
        xTaskCreatePinnedToCore(calculatorTask, "Calculator", TASK_STACK_SIZE, NULL, 2, &taskHandle, 1);
    }
}

void calculatorStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        DBG_PRINTLN("[Calculator] Stopped");
    }
}

bool calculatorIsRunning() {
    return isRunningFlag && (millis() - lastHeartbeat) < 3000;
}