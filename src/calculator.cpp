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
#include "task_common.h"

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
#if DEBUG_LOG
            Serial.println("[Calculator] Engine started, trip counters reset");
#endif
        }
        if (!pack.engine_running && engineRunning) {
            // Двигатель заглушён — пересчитываем avg_total
            if (avg_total == 0.0f || avg_cur == 0.0f) {
                avg_total = avg_cur;
            } else {
                avg_total = (avg_total + avg_cur) / 2.0f;
            }
#if DEBUG_LOG
            Serial.printf("[Calculator] Engine stopped, avg_total=%.1f\n", avg_total);
#endif
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
#if DEBUG_LOG
            Serial.printf("[Calculator] SettingsPack from storage: tank=%.1f\n", pack.tank_capacity);
#endif
        }
        float oldTank = tank_capacity;
        tank_capacity = pack.tank_capacity;
        if (oldTank != tank_capacity && fuel_base > tank_capacity) {
            fuel_base = tank_capacity;
#if DEBUG_LOG
            Serial.printf("[Calculator] Tank capacity changed: %.1f -> %.1f L, fuel_base corrected\n",
                          oldTank, tank_capacity);
#endif
        }
    }
}

// =============================================================================
// processCorrectOdo: Чтение нового значения ODO (int)
// =============================================================================
static void processCorrectOdo(QueueHandle_t q) {
    int newOdo;
    if (xQueueReceive(q, &newOdo, 0) == pdTRUE) {
#if DEBUG_LOG
        Serial.printf("[Calculator] ODO corrected: %d km\n", newOdo);
#endif
        odo_base = (double)newOdo;
    }
}

// =============================================================================
// processCommands: Обработка специфичных команд (вызывается из task_common)
// =============================================================================
static bool calcSpecificCmd(uint8_t cmd) {
    switch ((Command)cmd) {
        case CMD_RESET_TRIP_A:
            trip_a_base = -current_distance;
            fuel_trip_a_base = -current_fuel_used;
#if DEBUG_LOG
            Serial.println("[Calculator] Trip A reset");
#endif
            break;

        case CMD_RESET_TRIP_B:
            trip_b_base = -current_distance;
            fuel_trip_b_base = -current_fuel_used;
#if DEBUG_LOG
            Serial.println("[Calculator] Trip B reset");
#endif
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
            return false;  // Не обработана — пусть task_common логирует
    }
    return true;
}

// =============================================================================
// calculatorTask — Главная задача FreeRTOS
// =============================================================================

void calculatorTask(void* parameter) {
    (void)parameter;

    TaskContext ctx;
    if (!taskInit(&ctx, "Calculator", &isRunningFlag, &lastHeartbeat)) return;

    DataRouter& dr = DataRouter::getInstance();

    // Создаём очереди и регистрируем в DataRouter
    QueueHandle_t engineQ     = xQueueCreate(1, sizeof(EnginePack));
    QueueHandle_t tripQ       = xQueueCreate(1, sizeof(TripPack));
    QueueHandle_t settingsQ   = xQueueCreate(1, sizeof(SettingsPack));
    QueueHandle_t correctOdoQ = xQueueCreate(1, sizeof(int));

    dr.subscribe(TOPIC_ENGINE_PACK,    engineQ,     QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRIP_PACK,      tripQ,       QueuePolicy::OVERWRITE, true);   // retain
    dr.subscribe(TOPIC_SETTINGS_PACK,  settingsQ,   QueuePolicy::OVERWRITE, true);   // retain
    dr.subscribe(TOPIC_CORRECT_ODO,    correctOdoQ, QueuePolicy::OVERWRITE);

#if DEBUG_LOG
    Serial.println("[Calculator] Task started (DataRouter-based)");
#endif

    unsigned long lastPublish = 0;
    const unsigned long PUBLISH_INTERVAL = 1000;

    while (1) {
        taskHeartbeat(&ctx);

        // Читаем все доступные сообщения из очередей
        if (engineQ)      processEnginePack(engineQ);
        if (tripQ)        processTripPack(tripQ);
        if (settingsQ)    processSettingsPack(settingsQ);
        if (correctOdoQ)  processCorrectOdo(correctOdoQ);
        taskProcessCommands(&ctx, calcSpecificCmd);

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
        xTaskCreatePinnedToCore(calculatorTask, "Calculator", 4096, NULL, 2, &taskHandle, 1);
#if DEBUG_LOG
        Serial.println("[Calculator] Started");
#endif
    }
}

void calculatorStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
#if DEBUG_LOG
        Serial.println("[Calculator] Stopped");
#endif
    }
}

bool calculatorIsRunning() {
    return isRunningFlag && (millis() - lastHeartbeat) < 3000;
}

