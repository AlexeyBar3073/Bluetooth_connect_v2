// -----------------------------------------------------------------------------
// simulator_task.cpp
// Виртуальный двигатель автомобиля (симулятор физики).
//
// Назначение:
// - Эмулирует работу двигателя: кнопка запуска (GPIO26), педаль газа (GPIO33)
// - Кнопка габаритов (GPIO27): любое нажатие ≥50мс = переключение габаритов
// - Публикует EnginePack каждые 100 мс в TOPIC_ENGINE_PACK
// - Подписан на TOPIC_CMD (QUEUE_FIFO_DROP, depth=5) для команды full_tank
// - Подписан на TOPIC_SETTINGS_PACK (QUEUE_OVERWRITE, depth=1, retain=true)
//
// Физика:
// - Инерция скорости: разгон 0-100 км/ч за 10 сек (ΔV = 0.2 км/ч за 20мс)
// - RPM: кусочно-линейная кривая (750 idle → 6500 max)
// - Расход: 5 + (speed/100)*10 л/100км
// - Напряжение: 12.7В (выкл) / 13.5-14.5В (вкл)
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ Можно:
//   - Менять формулы физики (разгон, RPM, расход)
//   - Менять пины кнопок и потенциометра
//   - Добавлять новые эмулируемые параметры в EnginePack
//
// ❌ Нельзя:
//   - Вызывать Calculator/VehicleModel напрямую (только через DataBus)
//   - Публиковать данные чаще 100 мс
//   - Писать в NVS напрямую
//   - Забывать сбрасывать distance/fuel_used при запуске двигателя
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Публикация EnginePack, queue-подписка на команды
// -----------------------------------------------------------------------------

#include "simulator_task.h"
#include "data_bus.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"

// =============================================================================
// Глобальные переменные (внутреннее состояние модуля)
// =============================================================================

static TaskHandle_t  simulatorTaskHandle = NULL;
static bool          isRunning            = false;
static unsigned long lastHeartbeat        = 0;

// --- Физика ---
static bool   engineRunning  = false;
static bool   parkingLights  = false;  // Габаритные огни
static float  currentSpeed   = 0.0f;
static float  currentRpm     = 0.0f;
static float  currentThrottle = 0.0f;

// --- Накопленные за поездку ---
static float  currentDistance  = 0.0f;
static float  currentFuelUsed  = 0.0f;

// --- Топливо ---
static float  fuelBase         = 60.0f;   // Начальный остаток (от Storage/TripPack)
static float  fuelLevelSensor  = 60.0f;   // Текущий остаток (рассчитан, округлён до 0.1)

// --- Настройки (из SettingsPack) ---
static float  tankCapacity     = 60.0f;

// --- Фильтр АЦП ---
static float  filteredRaw      = 0.0f;
static const float ALPHA       = 0.2f;
static bool   pedalConnected   = false;  // true = потенциометр подключён

// --- Таймеры ---
static unsigned long lastPhysicsTime  = 0;
static unsigned long lastPublishTime  = 0;
static unsigned long lastPotReadTime  = 0;

// --- Кнопки ---
#define BUTTON_ENGINE_PIN   26
#define BUTTON_LIGHTS_PIN   27
#define LONG_PRESS_MS       800
#define LIGHTS_DEBOUNCE_MS  50

static volatile bool btnEngineTriggered = false;
static volatile bool btnLightsTriggered = false;

// =============================================================================
// ISR: Кнопки
// =============================================================================

void IRAM_ATTR engineButtonISR() {
    btnEngineTriggered = true;
}

void IRAM_ATTR lightsButtonISR() {
    btnLightsTriggered = true;
}

// =============================================================================
// Вспомогательные функции расчёта
// =============================================================================

static float getRpm() {
    if (!engineRunning) return 0.0f;
    if (currentSpeed <= 0.1f) return RPM_IDLE;
    if (currentSpeed <= 60.0f) return RPM_IDLE + (currentSpeed * 12.5f);
    return 1500.0f + ((currentSpeed - 60.0f) * 25.0f);
}

static float getVoltage() {
    if (engineRunning) return 13.5f + (random(0, 100) / 100.0f);
    return 12.7f;
}

static float getInstantFuel() {
    if (currentSpeed > 5.0f) return 5.0f + (currentSpeed / 100.0f) * 10.0f;
    return 0.0f;
}

// =============================================================================
// processCommands: Обработка очереди команд (TOPIC_CMD)
// =============================================================================
//
// Читает все доступные сообщения из очереди команд.
// Обрабатывает CMD_FULL_TANK: fuel_base = tank_capacity.
//
static void processCommands(QueueHandle_t cmdQueue) {
    BusMessage msg;
    while (xQueueReceive(cmdQueue, &msg, 0) == pdTRUE) {
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

        if (cmd == CMD_FULL_TANK) {
            fuelBase = tankCapacity;
            currentFuelUsed = 0.0f;
            fuelLevelSensor = roundf(fuelBase * 10.0f) / 10.0f;
            Serial.printf("[Simulator] Full tank: fuelBase = %.1f L\n", fuelBase);
        }
        busMessageFree(&msg);
    }
}

// =============================================================================
// simulatorTask — Главная задача FreeRTOS
// =============================================================================
//
// Цикл:
// 1. Обработка кнопок (GPIO26 двигатель, GPIO27 габариты)
// 2. Опрос потенциометра (GPIO33, каждые 20 мс)
// 3. Расчёт физики (каждые 20 мс)
// 4. Публикация EnginePack (каждые 100 мс)
// 5. Обработка команд из очереди TOPIC_CMD
// 6. Чтение настроек из очереди TOPIC_SETTINGS_PACK

void simulatorTask(void* parameter) {
    (void)parameter;
    isRunning = true;
    DataBus& db = DataBus::getInstance();

    // Инициализация GPIO
    pinMode(BUTTON_ENGINE_PIN, INPUT_PULLUP);
    pinMode(BUTTON_LIGHTS_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_ENGINE_PIN), engineButtonISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_LIGHTS_PIN), lightsButtonISR, FALLING);

    // Инициализация ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // Калибровка педали газа: 20 замеров, проверка стабильности
    // Если разброс > 200 — пин плавает (нет подтяжки), считаем отключённым
    {
        int sum = 0, minV = 4095, maxV = 0;
        for (int i = 0; i < 20; i++) {
            int v = analogRead(POTENTIOMETER_PIN);
            sum += v;
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
            delay(2);
        }
        int avg = sum / 20;
        int spread = maxV - minV;
        pedalConnected = (spread < 200 && avg < 4000);
        Serial.printf("[Simulator] Pedal ADC: avg=%d spread=%d %s\n",
                      avg, spread, pedalConnected ? "CONNECTED" : "DISCONNECTED (floating)");
        if (!pedalConnected) {
            filteredRaw = 0.0f;  // Принудительный 0 throttle
        } else {
            filteredRaw = (float)avg;
        }
    }

    // Подписка на команды (FIFO_DROP, depth=5)
    SubscriberOpts cmdOpts = {QUEUE_FIFO_DROP, 5, false};
    QueueHandle_t cmdQueue = db.subscribe(TOPIC_CMD, cmdOpts);

    // Подписка на TripPack (OVERWRITE, depth=1, retain=true) — для fuel_base
    SubscriberOpts tripOpts = {QUEUE_OVERWRITE, 1, true};
    QueueHandle_t tripQ = db.subscribe(TOPIC_TRIP_PACK, tripOpts);

    // Подписка на настройки (OVERWRITE, depth=1, retain=true)
    SubscriberOpts cfgOpts = {QUEUE_OVERWRITE, 1, true};
    QueueHandle_t cfgQueue = db.subscribe(TOPIC_SETTINGS_PACK, cfgOpts);

    // Публикация not_fuel = true (виртуальный расчёт топлива)
    // Это поле теперь в EnginePack, а не в отдельном топике.

    Serial.println("[Simulator] Task started (Queue-based)");

    // Локальные переменные для обработки кнопок
    unsigned long enginePressStartTime = 0;
    bool isHandlingEnginePress = false;
    unsigned long lightsPressStartTime = 0;
    bool isHandlingLightsPress = false;

    // Таймер публикации EnginePack
    unsigned long lastEnginePackPublish = 0;

    while (1) {
        lastHeartbeat = millis();
        unsigned long now = millis();

        // ====== 1. Кнопка двигателя (GPIO26) ======
        int enginePinState = digitalRead(BUTTON_ENGINE_PIN);
        if (btnEngineTriggered && !isHandlingEnginePress) {
            btnEngineTriggered = false;
            if (enginePinState == LOW) {
                isHandlingEnginePress = true;
                enginePressStartTime = now;
            }
        }
        if (isHandlingEnginePress) {
            if (enginePinState == HIGH) {
                isHandlingEnginePress = false;
            } else if ((now - enginePressStartTime) >= LONG_PRESS_MS) {
                engineRunning = !engineRunning;
                if (engineRunning) {
                    currentDistance  = 0.0f;
                    currentFuelUsed  = 0.0f;
                    Serial.println("[Simulator] Engine STARTED");
                } else {
                    Serial.println("[Simulator] Engine STOPPED");
                }
                isHandlingEnginePress = false;
            }
        }

        // ====== 2. Кнопка габаритов (GPIO27) ======
        int lightsPinState = digitalRead(BUTTON_LIGHTS_PIN);
        if (btnLightsTriggered && !isHandlingLightsPress) {
            btnLightsTriggered = false;
            if (lightsPinState == LOW) {
                isHandlingLightsPress = true;
                lightsPressStartTime = now;
            }
        }
        if (isHandlingLightsPress) {
            if (lightsPinState == HIGH) {
                unsigned long duration = now - lightsPressStartTime;
                if (duration >= LIGHTS_DEBOUNCE_MS) {
                    parkingLights = !parkingLights;
                    Serial.printf("[Simulator] Parking lights: %s\n", parkingLights ? "ON" : "OFF");
                }
                isHandlingLightsPress = false;
            }
        }

        // ====== 3. Опрос потенциометра (каждые 20 мс) ======
        if (now - lastPotReadTime >= 20) {
            lastPotReadTime = now;
            int raw = analogRead(POTENTIOMETER_PIN);
            filteredRaw = ALPHA * raw + (1.0f - ALPHA) * filteredRaw;
            float targetThrottle = filteredRaw / 4095.0f;

            float delta = targetThrottle - currentThrottle;
            float maxDelta = (0.1f / 1000.0f) * 20;
            currentThrottle += (delta > 0 ? maxDelta : -maxDelta);
            currentThrottle = constrain(currentThrottle, 0.0f, 1.0f);
        }

        // ====== 4. Физика (каждые 20 мс) ======
        if (now - lastPhysicsTime >= 20) {
            lastPhysicsTime = now;

            if (engineRunning) {
                float targetSpeed = currentThrottle * SPEED_MAX;
                float step = (SPEED_CHANGE_PER_SEC / 1000.0f) * 20;

                if (abs(targetSpeed - currentSpeed) <= step) currentSpeed = targetSpeed;
                else currentSpeed += (targetSpeed > currentSpeed ? step : -step);
                currentSpeed = max(0.0f, currentSpeed);

                currentRpm = getRpm();

                // Пробег за 20 мс
                float dt_hours = 0.02f / 3600.0f;
                currentDistance += currentSpeed * dt_hours;

                // Расход за 20 мс
                float instantLph = getInstantFuel();
                float dist_km = currentSpeed * dt_hours;
                currentFuelUsed += (instantLph * dist_km) / 100.0f;
            } else {
                float step = (SPEED_CHANGE_PER_SEC / 1000.0f) * 20;
                currentSpeed = max(0.0f, currentSpeed - step);
                currentRpm = 0.0f;
            }
        }

        // ====== 5. Публикация EnginePack (каждые 100 мс) ======
        if (now - lastPublishTime >= 100) {
            lastPublishTime = now;

            // Расчёт остатка топлива: fuel_base - fuel_used, округление до 0.1
            float fuelCalc = fuelBase - currentFuelUsed;
            if (fuelCalc < 0.0f) fuelCalc = 0.0f;
            fuelLevelSensor = roundf(fuelCalc * 10.0f) / 10.0f;

            EnginePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version           = 1;
            pack.speed             = currentSpeed;
            pack.rpm               = roundf(currentRpm / 10.0f) * 10.0f;
            pack.voltage           = getVoltage();
            pack.engine_running    = engineRunning;
            pack.parking_lights    = parkingLights;
            pack.instant_fuel      = getInstantFuel();
            pack.distance          = currentDistance;
            pack.fuel_used         = currentFuelUsed;
            pack.fuel_level_sensor = fuelLevelSensor;  // Округлён до 0.1 л
            pack.not_fuel          = true;             // Датчика нет, Simulator считает сам
            pack.gear              = 0;
            strcpy(pack.selector_pos, "D");
            pack.tcc_lockup        = false;

            db.publishPacket(TOPIC_ENGINE_PACK, &pack, sizeof(pack));
        }

        // ====== 6. Обработка команд ======
        if (cmdQueue) processCommands(cmdQueue);

        // ====== 7. Чтение TripPack (fuel_base) ======
        BusMessage msg;
        if (tripQ && xQueueReceive(tripQ, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
            TripPack p;
            memcpy(&p, msg.value.s, sizeof(TripPack));
            if (p.fuel_level > 0 && fuelBase < 0.1f) {
                fuelBase = p.fuel_level;
                fuelLevelSensor = fuelBase;
                Serial.printf("[Simulator] fuel_base from Storage: %.1f L\n", fuelBase);
            }
            busMessageFree(&msg);
        }

        // ====== 8. Чтение настроек ======
        if (cfgQueue) {
            BusMessage msg;
            if (xQueueReceive(cfgQueue, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
                SettingsPack pack;
                memcpy(&pack, msg.value.s, sizeof(SettingsPack));
                tankCapacity = pack.tank_capacity;
                busMessageFree(&msg);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void simulatorStart() {
    if (!simulatorTaskHandle) {
        xTaskCreatePinnedToCore(
            simulatorTask, "Simulator", 4096, NULL,
            TASK_PRIORITY_SIMULATOR, &simulatorTaskHandle, 0
        );
        isRunning = true;
        Serial.println("[Simulator] Started");
    }
}

void simulatorStop() {
    if (simulatorTaskHandle) {
        vTaskDelete(simulatorTaskHandle);
        simulatorTaskHandle = NULL;
        isRunning = false;
        Serial.println("[Simulator] Stopped");
    }
}

bool simulatorIsRunning() {
    return isRunning && (millis() - lastHeartbeat) < 3000;
}

void simulatorSetSpeed(float speed) { currentSpeed = speed; }
void simulatorSetFuel(float fuel) { (void)fuel; }
void simulatorToggleEngine() { engineRunning = !engineRunning; }
bool simulatorIsEngineRunning() { return engineRunning; }

