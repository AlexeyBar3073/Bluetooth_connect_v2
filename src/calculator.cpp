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
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, TripPack
// -----------------------------------------------------------------------------

#include "calculator.h"
#include "data_router.h"

// =============================================================================
// Глобальные переменные
// =============================================================================

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;

// --- Base-значения (от Storage, не меняются до команд) ---
static double  odo_base          = 0.0;
static float   trip_a_base       = 0.0f;
static float   trip_b_base       = 0.0f;
static float   fuel_trip_a_base  = 0.0f;
static float   fuel_trip_b_base  = 0.0f;
static float   fuel_base         = 60.0f;

// --- Инициализация от Storage ---
static bool    storageInit       = false;

// --- Накопленные за поездку ---
static float   current_distance  = 0.0f;
static float   current_fuel_used = 0.0f;

// --- Средний расход ---
static float   avg_cur           = 0.0f;   // За текущую поездку
static float   avg_total         = 0.0f;   // Накопленный за всё время

// --- Топливо от датчика (EnginePack) ---
static float   fuel_level_sensor = 0.0f;  // Рассчитан EngineModule/Simulator

// --- Статусы ---
static bool    not_fuel          = false;  // true = датчика топлива нет (из EnginePack.not_fuel)
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
            Serial.println("[Calculator] Engine started, trip counters reset");
        }
        if (!pack.engine_running && engineRunning) {
            // Двигатель заглушён — пересчитываем avg_total
            if (avg_total == 0.0f || avg_cur == 0.0f) {
                avg_total = avg_cur;
            } else {
                avg_total = (avg_total + avg_cur) / 2.0f;
            }
            Serial.printf("[Calculator] Engine stopped, avg_total=%.1f\n", avg_total);
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
        if (not_fuel && !engineRunning) fuel_base = pack.fuel_level;
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
            Serial.printf("[Calculator] SettingsPack from storage: tank=%.1f\n", pack.tank_capacity);
        }
        float oldTank = tank_capacity;
        tank_capacity = pack.tank_capacity;
        if (oldTank != tank_capacity && fuel_base > tank_capacity) {
            fuel_base = tank_capacity;
            Serial.printf("[Calculator] Tank capacity changed: %.1f -> %.1f L, fuel_base corrected\n",
                          oldTank, tank_capacity);
        }
    }
}

// =============================================================================
// processCorrectOdo: Чтение нового значения ODO (double)
// =============================================================================
static void processCorrectOdo(QueueHandle_t q) {
    double newOdo;
    if (xQueueReceive(q, &newOdo, 0) == pdTRUE) {
        Serial.printf("[Calculator] ODO corrected: %.1f km\n", newOdo);
        odo_base = newOdo;
    }
}

// =============================================================================
// processCommands: Обработка команд
// =============================================================================
static void processCommands(QueueHandle_t q) {
    uint8_t cmd;
    while (xQueueReceive(q, &cmd, 0) == pdTRUE) {
        switch ((Command)cmd) {
            case CMD_RESET_TRIP_A:
                trip_a_base = -current_distance;
                fuel_trip_a_base = -current_fuel_used;
                Serial.println("[Calculator] Trip A reset");
                break;

            case CMD_RESET_TRIP_B:
                trip_b_base = -current_distance;
                fuel_trip_b_base = -current_fuel_used;
                Serial.println("[Calculator] Trip B reset");
                break;

            case CMD_RESET_AVG:
                current_distance  = 0.0f;
                current_fuel_used = 0.0f;
                avg_cur = 0.0f;
                break;

            case CMD_FULL_TANK:
                fuel_base = tank_capacity;
                current_fuel_used = 0.0f;
                break;

            default:
                break;
        }
    }
}

// =============================================================================
// calculatorTask — Главная задача FreeRTOS
// =============================================================================

void calculatorTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& dr = DataRouter::getInstance();

    // Создаём очереди и регистрируем в DataRouter
    QueueHandle_t engineQ     = xQueueCreate(1, sizeof(EnginePack));
    QueueHandle_t tripQ       = xQueueCreate(1, sizeof(TripPack));
    QueueHandle_t settingsQ   = xQueueCreate(1, sizeof(SettingsPack));
    QueueHandle_t correctOdoQ = xQueueCreate(1, sizeof(double));
    QueueHandle_t cmdQ        = xQueueCreate(5, sizeof(uint8_t));

    dr.subscribe(TOPIC_ENGINE_PACK,    engineQ,     QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRIP_PACK,      tripQ,       QueuePolicy::OVERWRITE, true);   // retain
    dr.subscribe(TOPIC_SETTINGS_PACK,  settingsQ,   QueuePolicy::OVERWRITE, true);   // retain
    dr.subscribe(TOPIC_CORRECT_ODO,    correctOdoQ, QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_CMD,            cmdQ,        QueuePolicy::FIFO_DROP);

    Serial.println("[Calculator] Task started (DataRouter-based)");

    unsigned long lastPublish = 0;
    const unsigned long PUBLISH_INTERVAL = 1000;

    while (1) {
        lastHeartbeat = millis();

        // Читаем все доступные сообщения из очередей
        if (engineQ)      processEnginePack(engineQ);
        if (tripQ)        processTripPack(tripQ);
        if (settingsQ)    processSettingsPack(settingsQ);
        if (correctOdoQ)  processCorrectOdo(correctOdoQ);
        if (cmdQ)         processCommands(cmdQ);

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
        xTaskCreatePinnedToCore(calculatorTask, "Calculator", 4096, NULL, 2, &taskHandle, 0);
        Serial.println("[Calculator] Started");
    }
}

void calculatorStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        Serial.println("[Calculator] Stopped");
    }
}

bool calculatorIsRunning() {
    return isRunningFlag && (millis() - lastHeartbeat) < 3000;
}

