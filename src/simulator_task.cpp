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
//   - Вызывать Calculator/VehicleModel напрямую (только через DataRouter)
//   - Публиковать данные чаще 100 мс
//   - Писать в NVS напрямую
//   - Забывать сбрасывать distance/fuel_used при запуске двигателя
//
// Обновление (интеграция с task_common):
// - Использует taskInit() для инициализации и подписки на TOPIC_CMD
// - Регистрирует подписки через taskRegisterSubscription()
// - taskProcessCommands() обрабатывает CMD_OTA_START с полной очисткой ресурсов
// - taskHeartbeat() обновляет счётчик для loop()-мониторинга
// - При OTA все очереди удаляются, подписки снимаются, задача завершается
// - Освобождаются аппаратные ресурсы: GPIO ISR, ADC
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include "simulator_task.h"
#include "data_router.h"
#include "task_common.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"
#include "debug.h"

// =============================================================================
// Глобальные переменные (внутреннее состояние модуля)
// =============================================================================

static TaskHandle_t  simulatorTaskHandle = NULL;
static bool          isRunning            = false;
static unsigned long lastHeartbeat        = 0;

// --- Контекст задачи (фреймворк task_common) ---
// Внедрение фреймворка task_common:
//   - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
//   - Единый heartbeat для мониторинга в loop()
//   - Регистрация подписок для корректной отписки при завершении
static TaskContext ctx = {0};

// --- Очереди для подписки на данные ---
// Создаются модулем, регистрируются через taskRegisterSubscription()
static QueueHandle_t cmdQueue = NULL;
static QueueHandle_t tripQ    = NULL;
static QueueHandle_t cfgQ     = NULL;

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

// --- Инициализация от Storage ---
static bool   storageInit      = false;
static bool   fuelLoaded       = false;

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
// simCmdHandler — обработка специфичных команд модуля Simulator
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
static bool simCmdHandler(uint8_t cmd) {
    switch ((Command)cmd) {
        case CMD_FULL_TANK:
            fuelBase = tankCapacity;
            currentFuelUsed = 0.0f;
            fuelLevelSensor = roundf(fuelBase * 10.0f) / 10.0f;
            DBG_PRINTF("[Simulator] Full tank: fuelBase = %.1f L\n", fuelBase);
            return true;

        case CMD_OTA_START:
            return true;  // Завершить задачу при OTA

        default:
            return false;  // Команда не распознана
    }
}

// =============================================================================
// simPreShutdownCleanup — освобождение аппаратных ресурсов перед OTA
// =============================================================================
// Вызывается из taskProcessCommands() при получении CMD_OTA_START
// ДО вызова taskShutdown().
//
// Освобождает:
//   - GPIO ISR (detachInterrupt)
//   - Выключает ADC (экономия энергии)
//
// ВАЖНО: эта функция НЕ должна вызывать taskShutdown или vTaskDelete,
// она только освобождает специфичные для модуля аппаратные ресурсы.
//
static void simPreShutdownCleanup(void) {
    DBG_PRINTLN("[Simulator] Pre-shutdown cleanup: detaching interrupts...");
    
    // Отключаем прерывания кнопок
    detachInterrupt(digitalPinToInterrupt(BUTTON_ENGINE_PIN));
    detachInterrupt(digitalPinToInterrupt(BUTTON_LIGHTS_PIN));
    
    // Сбрасываем флаги
    btnEngineTriggered = false;
    btnLightsTriggered = false;
    
    DBG_PRINTLN("[Simulator] Hardware resources released");
}

// =============================================================================
// simCmdHandlerWithCleanup — расширенный обработчик с очисткой
// =============================================================================
// Обёртка над simCmdHandler, которая также вызывает очистку аппаратных ресурсов
// при получении CMD_OTA_START.
//
static bool simCmdHandlerWithCleanup(uint8_t cmd) {
    if ((Command)cmd == CMD_OTA_START) {
        // Освобождаем аппаратные ресурсы ДО вызова taskShutdown()
        simPreShutdownCleanup();
        // Возвращаем true — фреймворк продолжит обработку и вызовет taskShutdown()
        return true;
    }
    
    // Для остальных команд — обычный обработчик
    return simCmdHandler(cmd);
}

// =============================================================================
// simulatorTask — Главная задача FreeRTOS
// =============================================================================
//
// Архитектура на основе task_common:
//   1. taskInit() — инициализация, создание cmdQ, подписка на TOPIC_CMD
//   2. Инициализация GPIO и ADC
//   3. Создание очередей и подписка на топики данных
//   4. Основной цикл:
//      - taskHeartbeat() — обновление счётчика активности
//      - Обработка кнопок (GPIO26 двигатель, GPIO27 габариты)
//      - Опрос потенциометра (GPIO33, каждые 20 мс)
//      - Расчёт физики (каждые 20 мс)
//      - Публикация EnginePack (каждые 100 мс)
//      - taskProcessCommands() — обработка команд (включая OTA)
//      - Чтение настроек из очереди TOPIC_SETTINGS_PACK
//
// При получении CMD_OTA_START:
//   - simCmdHandlerWithCleanup() вызывает simPreShutdownCleanup()
//   - taskProcessCommands() вызывает taskShutdown()
//   - taskShutdown() отписывается от топиков, удаляет очереди, завершает задачу
//   - Память полностью освобождается для OTA-обновления
//
void simulatorTask(void* parameter) {
    (void)parameter;
    
    // === ИНИЦИАЛИЗАЦИЯ ЧЕРЕЗ ФРЕЙМВОРК ===
    // taskInit() выполняет:
    //   - Установку isRunningFlag = true
    //   - Создание cmdQ (очередь команд)
    //   - Подписку на TOPIC_CMD
    //   - Регистрацию подписки для автоматической очистки при shutdown
    if (!taskInit(&ctx, "Simulator", &isRunning, &lastHeartbeat)) {
        DBG_PRINTLN("[Simulator] ERROR: taskInit failed!");
        isRunning = false;
        vTaskDelete(NULL);
        return;
    }
    
    // Сохраняем указатель на cmdQueue для обратной совместимости
    cmdQueue = ctx.cmdQ;

    // Инициализация GPIO
    pinMode(BUTTON_ENGINE_PIN, INPUT_PULLUP);
    pinMode(BUTTON_LIGHTS_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_ENGINE_PIN), engineButtonISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_LIGHTS_PIN), lightsButtonISR, FALLING);

    // Инициализация ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // Калибровка педали газа
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
        if (!pedalConnected) {
            filteredRaw = 0.0f;
        } else {
            filteredRaw = (float)avg;
        }
    }

    DataRouter& dr = DataRouter::getInstance();

    // === СОЗДАНИЕ ОЧЕРЕДЕЙ И ПОДПИСКА НА ТОПИКИ ===
    // Очереди создаются модулем и регистрируются в фреймворке для
    // автоматической очистки при завершении задачи.
    tripQ = xQueueCreate(1, sizeof(TripPack));
    cfgQ  = xQueueCreate(1, sizeof(SettingsPack));

    if (!tripQ || !cfgQ) {
        DBG_PRINTLN("[Simulator] ERROR: Failed to create queues!");
        isRunning = false;
        vTaskDelete(NULL);
        return;
    }

    dr.subscribe(TOPIC_TRIP_PACK, tripQ, QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_SETTINGS_PACK, cfgQ, QueuePolicy::OVERWRITE, true);  // retain

    // Регистрируем подписки для автоматической отписки при shutdown
    taskRegisterSubscription(&ctx, TOPIC_TRIP_PACK, tripQ);
    taskRegisterSubscription(&ctx, TOPIC_SETTINGS_PACK, cfgQ);

    // Локальные переменные для обработки кнопок
    unsigned long enginePressStartTime = 0;
    bool isHandlingEnginePress = false;
    unsigned long lightsPressStartTime = 0;
    bool isHandlingLightsPress = false;

    // Таймер публикации EnginePack
    unsigned long lastEnginePackPublish = 0;

    while (1) {
        // Heartbeat — обновление счётчика для loop()-мониторинга
        taskHeartbeat(&ctx);
        
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
                    DBG_PRINTLN("[Simulator] Engine STARTED");
                } else {
                    DBG_PRINTLN("[Simulator] Engine STOPPED");
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
                    DBG_PRINTF("[Simulator] Parking lights: %s\n", parkingLights ? "ON" : "OFF");
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
        if (now - lastEnginePackPublish >= 100) {
            lastEnginePackPublish = now;

            // Расчёт остатка топлива: fuel_base - fuel_used, округление до 0.1
            float fuelCalc = fuelBase - currentFuelUsed;
            if (fuelCalc < 0.0f) fuelCalc = 0.0f;
            fuelLevelSensor = roundf(fuelCalc * 10.0f) / 10.0f;

            EnginePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version           = 3;
            pack.speed             = currentSpeed;
            pack.rpm               = roundf(currentRpm / 10.0f) * 10.0f;
            pack.voltage           = getVoltage();
            pack.engine_running    = engineRunning;
            pack.parking_lights    = parkingLights;
            pack.instant_fuel      = getInstantFuel();
            pack.distance          = currentDistance;
            pack.fuel_used         = currentFuelUsed;
            pack.fuel_level_sensor = fuelLevelSensor;
            pack.not_fuel          = true;

            dr.publishPacket(TOPIC_ENGINE_PACK, &pack, sizeof(pack));
        }

        // ====== 6. Обработка команд ======
        // Используем расширенный обработчик с очисткой аппаратных ресурсов
        taskProcessCommands(&ctx, simCmdHandlerWithCleanup);

        // ====== 7. Чтение TripPack (fuel_base из NVS) ======
        if (tripQ) {
            TripPack p;
            if (xQueueReceive(tripQ, &p, 0) == pdTRUE) {
                // Берём fuel_level из первого TripPack (из NVS)
                if (!fuelLoaded && p.fuel_level > 0.01f) {
                    fuelLoaded = true;
                    fuelBase = p.fuel_level;
                    fuelLevelSensor = fuelBase;
                }
            }
        }

        // ====== 8. Чтение настроек ======
        if (cfgQ) {
            SettingsPack pack;
            if (xQueueReceive(cfgQ, &pack, 0) == pdTRUE) {
                if (!storageInit) {
                    storageInit = true;
                }
                tankCapacity = pack.tank_capacity;
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
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunning = true;
        // Ядро 0 — Simulator (физика автомобиля)
        xTaskCreatePinnedToCore(
            simulatorTask, "Simulator", TASK_STACK_SIZE, NULL,
            TASK_PRIORITY_SIMULATOR, &simulatorTaskHandle, 0
        );
        isRunning = true;
    }
}

void simulatorStop() {
    if (simulatorTaskHandle) {
        // Отключаем прерывания перед удалением задачи
        detachInterrupt(digitalPinToInterrupt(BUTTON_ENGINE_PIN));
        detachInterrupt(digitalPinToInterrupt(BUTTON_LIGHTS_PIN));
        
        vTaskDelete(simulatorTaskHandle);
        simulatorTaskHandle = NULL;
        isRunning = false;
        DBG_PRINTLN("[Simulator] Stopped");
    }
}

bool simulatorIsRunning() {
    return isRunning && (millis() - lastHeartbeat) < 3000;
}

void simulatorSetSpeed(float speed) { currentSpeed = speed; }
void simulatorSetFuel(float fuel) { (void)fuel; }
void simulatorToggleEngine() { engineRunning = !engineRunning; }
bool simulatorIsEngineRunning() { return engineRunning; }