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
#include "data_bus.h"

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
static float   avg_stored        = 0.0f;

// --- Накопленные за поездку ---
static float   current_distance  = 0.0f;
static float   current_fuel_used = 0.0f;

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
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
        EnginePack pack;
        memcpy(&pack, msg.value.s, sizeof(EnginePack));

        if (pack.engine_running && !engineRunning) {
            current_distance  = 0.0f;
            current_fuel_used = 0.0f;
        }
        engineRunning = pack.engine_running;
        current_distance  = pack.distance;
        current_fuel_used = pack.fuel_used;
        fuel_level_sensor = pack.fuel_level_sensor;
        not_fuel = pack.not_fuel;  // Датчик топлива присутствует/отсутствует
        busMessageFree(&msg);
    }
}

// =============================================================================
// processTripPack: Чтение TripPack от Storage (base-значения)
// =============================================================================
// ВАЖНО: обновляем base ТОЛЬКО если значение пришло извне (Storage, set_cfg).
// Не обновляем trip-базы из собственного TripPack — это вызовет квадратичный рост!
//
// Признак "извне": odo > 0 И двигатель ещё не запущен (первый запуск).
// После запуска двигателя — base НЕ меняется до команд reset_*.
//
static void processTripPack(QueueHandle_t q) {
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
        TripPack pack;
        memcpy(&pack, msg.value.s, sizeof(TripPack));

        // Обновляем base только при первом старте (engine ещё не running, odo_base ещё 0)
        // Это защищает от обновления своим же пакетом
        if (odo_base == 0.0 && pack.odo > 0.01 && !engineRunning) {
            odo_base = pack.odo;
            trip_a_base = pack.trip_a;
            trip_b_base = pack.trip_b;
            fuel_trip_a_base = pack.fuel_trip_a;
            fuel_trip_b_base = pack.fuel_trip_b;
            avg_stored = pack.avg_consumption;
            Serial.printf("[Calculator] TripPack base from storage: ODO=%.0f\n", odo_base);
        }

        // fuel_level берём из TripPack только если нет датчика (not_fuel = true)
        // и двигатель ещё не запущен — иначе будет цикл пересчёта
        if (not_fuel && !engineRunning) fuel_base = pack.fuel_level;

        busMessageFree(&msg);
    }
}

// =============================================================================
// processSettingsPack: Чтение настроек
// =============================================================================
static void processSettingsPack(QueueHandle_t q) {
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
        SettingsPack pack;
        memcpy(&pack, msg.value.s, sizeof(SettingsPack));

        float oldTank = tank_capacity;
        tank_capacity = pack.tank_capacity;

        // Если ёмкость бака изменилась — проверяем, что остаток не превышает новую ёмкость
        if (oldTank != tank_capacity && fuel_base > tank_capacity) {
            fuel_base = tank_capacity;
            Serial.printf("[Calculator] Tank capacity changed: %.1f -> %.1f L, fuel_base corrected\n",
                          oldTank, tank_capacity);
        }

        busMessageFree(&msg);
    }
}

// =============================================================================
// processCorrectOdo: Чтение нового значения ODO (float)
// =============================================================================
static void processCorrectOdo(QueueHandle_t q) {
    BusMessage msg;
    if (xQueueReceive(q, &msg, 0) == pdTRUE) {
        if (msg.type == TYPE_FLOAT) {
            double newOdo = (double)msg.value.f;
            Serial.printf("[Calculator] ODO corrected: %.1f km\n", newOdo);
            odo_base = newOdo;
        }
        busMessageFree(&msg);
    }
}

// =============================================================================
// processCommands: Обработка команд
// =============================================================================
static void processCommands(QueueHandle_t q) {
    BusMessage msg;
    while (xQueueReceive(q, &msg, 0) == pdTRUE) {
        // Protocol публикует простые команды как int, а параметрические — как CmdPayload
        Command cmd;
        if (msg.type == TYPE_INT) {
            cmd = (Command)msg.value.i;
        } else if (msg.type == TYPE_CMD) {
            cmd = msg.cmd.cmd;
        } else {
            busMessageFree(&msg);
            continue;
        }

        switch (cmd) {
            case CMD_RESET_TRIP_A:
                // trip_a = trip_a_base + current_distance. Чтобы trip_a = 0:
                trip_a_base = -current_distance;
                fuel_trip_a_base = -current_fuel_used;
                Serial.println("[Calculator] Trip A reset");
                break;

            case CMD_RESET_TRIP_B:
                // trip_b = trip_b_base + current_distance. Чтобы trip_b = 0:
                trip_b_base = -current_distance;
                fuel_trip_b_base = -current_fuel_used;
                Serial.println("[Calculator] Trip B reset");
                break;

            case CMD_RESET_AVG:
                current_distance  = 0.0f;
                current_fuel_used = 0.0f;
                avg_stored        = 0.0f;
                break;

            case CMD_FULL_TANK:
                fuel_base = tank_capacity;
                current_fuel_used = 0.0f;
                break;

            default:
                break;
        }
        busMessageFree(&msg);
    }
}

// =============================================================================
// calculatorTask — Главная задача FreeRTOS
// =============================================================================

void calculatorTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataBus& db = DataBus::getInstance();

    // Подписка на EnginePack (OVERWRITE, depth=1)
    SubscriberOpts engOpts = {QUEUE_OVERWRITE, 1, false};
    QueueHandle_t engineQ = db.subscribe(TOPIC_ENGINE_PACK, engOpts);

    // Подписка на TripPack от Storage (OVERWRITE, depth=1, retain=true)
    SubscriberOpts tripOpts = {QUEUE_OVERWRITE, 1, true};
    QueueHandle_t tripQ = db.subscribe(TOPIC_TRIP_PACK, tripOpts);

    // Подписка на SettingsPack (OVERWRITE, depth=1, retain=true)
    QueueHandle_t settingsQ = db.subscribe(TOPIC_SETTINGS_PACK, tripOpts);

    // Подписка на коррекцию ODO (OVERWRITE, depth=1)
    QueueHandle_t correctOdoQ = db.subscribe(TOPIC_CORRECT_ODO, tripOpts);

    // Подписка на команды (FIFO_DROP, depth=5)
    SubscriberOpts cmdOpts = {QUEUE_FIFO_DROP, 5, false};
    QueueHandle_t cmdQ = db.subscribe(TOPIC_CMD, cmdOpts);

    Serial.println("[Calculator] Task started (Queue-based)");

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
                // Датчика нет — считаем сами: fuel_base - fuel_used
                pack.fuel_level = max(0.0f, fuel_base - current_fuel_used);
            } else {
                // Датчик есть — пробрасываем от EngineModule/Simulator
                pack.fuel_level = fuel_level_sensor;
            }

            pack.avg_consumption = (current_distance > 0.001f) ? (current_fuel_used / current_distance) * 100.0f : 0.0f;

            db.publishPacket(TOPIC_TRIP_PACK, &pack, sizeof(pack));
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

