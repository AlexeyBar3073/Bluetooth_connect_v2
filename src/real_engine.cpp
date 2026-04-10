// -----------------------------------------------------------------------------
// real_engine.cpp
// Реальный двигатель: форсунка + датчик выходного вала (геркон).
//
// Форсунка:
//   - Оптопара инвертирует сигнал → считаем положительный фронт
//   - Суммируем длительность ВСЕХ открытий форсунки за период
//   - Расход: (injector_flow × injector_count / 60000) × duration_ms  → мл
//
// Датчик вала:
//   - 1 оборот геркона = 1 метр пути
//   - pulses_per_meter из настроек (по умолчанию 3.0)
//   - Расстояние: pulse_count / pulses_per_meter  → метры
//   - Скорость: distance_m / time_s × 3.6  → км/ч
//
// Пропуски форсунки:
//   - Если ISR не вызвана — потерянный импульс
//   - Отслеживаем: если длительность < 0.5 мс или > 50 мс — аномалия
//
// ВЕРСИЯ: 6.2.0 — Реальный двигатель (форсунка + геркон)
// -----------------------------------------------------------------------------

#include "real_engine.h"
#include "data_router.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"
#include "ina226.h"

// =============================================================================
// Пином
// =============================================================================

#define INJECTOR_PIN   4     // Форсунка (оптопара, положительный фронт)
#define SHAFT_PIN      13    // Датчик выходного вала (геркон)

// =============================================================================
// Конфигурация
// =============================================================================

#define MIN_INJECTOR_US    200    // Мин. длительность форсунки (мкс), ниже — шум
#define MAX_INJECTOR_US  50000    // Макс. длительность форсунки (50 мс), выше — пропуск

// =============================================================================
// Volatile данные из ISR
// =============================================================================

static volatile uint64_t injectorEdgeUs   = 0;     // Микросекунды последнего фронта
static volatile uint64_t injectorTotalUs  = 0;     // Сумма длительностей (мкс)
static volatile uint32_t injectorPulses  = 0;     // Количество открытий
static volatile uint32_t injectorMissed  = 0;     // Пропущенные (аномальная длительность)

static volatile uint32_t shaftPulseCount = 0;      // Импульсы датчика вала

// =============================================================================
// INA226 — напряжение бортсети + датчик уровня топлива (поплавок-реостат)
// =============================================================================

static INA226 ina226(0x40);  // Адрес I2C (A0=GND, A1=GND)
static bool   inaReady = false;

// Калибровка уровня топлива (нужно замерить при реальном баке):
//   Полный бак  → shunt_mV при 100%
//   Пустой бак  → shunt_mV при 0%
static float fuelShuntFull  = 50.0f;   // мВ на шунте при полном баке (TODO: замерить)
static float fuelShuntEmpty = 5.0f;    // мВ на шунте при пустом баке (TODO: замерить)

static float    tankCapacity   = 60.0f;
static float    injectorFlow   = 250.0f;   // мл/мин на форсунку
static uint8_t  injectorCount  = 4;
static float    pulsesPerMeter = 3.0f;

// =============================================================================
// Состояние двигателя (обновляется в задаче)
// =============================================================================

static bool     engineRunning   = false;
static bool     parkingLights   = false;
static float    fuelBase        = 60.0f;     // Базовый уровень топлива (л)
static float    fuelLevel       = 60.0f;     // Текущий уровень топлива (л)
static float    distanceAccum   = 0.0f;      // Накопленный пробег за поездку (км)
static float    fuelUsedAccum   = 0.0f;      // Накопленный расход за поездку (л)

// =============================================================================
// Очереди и задача
// =============================================================================

static TaskHandle_t  taskHandle    = NULL;
static bool          isRunningFlag = false;
static unsigned long lastHeartbeat = 0;

static QueueHandle_t cmdQ        = NULL;
static QueueHandle_t settingsQ   = NULL;
static QueueHandle_t tripQ       = NULL;

// =============================================================================
// ISR — Форсунка
// =============================================================================
//
// Положительный фронт (оптопара инвертирует):
//   Rising edge → запоминаем время
//   Falling edge (следующий rising) → считаем длительность
//
// ВАЖНО: внутри ISR нельзя Serial, malloc, delay!
//
static void IRAM_ATTR injectorISR() {
    uint64_t now = esp_timer_get_time();
    if (injectorEdgeUs == 0) {
        // Первый фронт — начало впрыска
        injectorEdgeUs = now;
    } else {
        // Второй фронт — конец впрыска
        uint32_t duration = (uint32_t)(now - injectorEdgeUs);
        if (duration >= MIN_INJECTOR_US && duration <= MAX_INJECTOR_US) {
            injectorTotalUs += duration;
            injectorPulses++;
        } else {
            injectorMissed++;  // Аномальная длительность — возможно пропуск
        }
        injectorEdgeUs = now;
    }
}

// =============================================================================
// ISR — Датчик выходного вала (геркон)
// =============================================================================

static void IRAM_ATTR shaftISR() {
    shaftPulseCount++;
}

// =============================================================================
// processCommands
// =============================================================================

static void processCommands() {
    uint8_t cmd;
    while (xQueueReceive(cmdQ, &cmd, 0) == pdTRUE) {
        if ((Command)cmd == CMD_FULL_TANK) {
            fuelBase = tankCapacity;
            fuelLevel = tankCapacity;
            Serial.printf("[RealEngine] Full tank: %.1f L\n", fuelLevel);
        }
    }
}

// =============================================================================
// processSettingsPack
// =============================================================================

static void processSettingsPack() {
    SettingsPack pack;
    if (xQueueReceive(settingsQ, &pack, 0) == pdTRUE) {
        tankCapacity   = pack.tank_capacity;
        injectorFlow   = pack.injector_flow;
        injectorCount  = pack.injector_count;
        pulsesPerMeter = pack.pulses_per_meter;
    }
}

// =============================================================================
// processTripPack
// =============================================================================

static void processTripPack() {
    TripPack pack;
    if (xQueueReceive(tripQ, &pack, 0) == pdTRUE) {
        // Берём fuel_level из TripPack для синхронизации при старте
        if (!engineRunning && pack.fuel_level > 0.01f) {
            fuelBase = pack.fuel_level;
            fuelLevel = pack.fuel_level;
        }
    }
}

// =============================================================================
// realEngineTask
// =============================================================================

static void realEngineTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& router = DataRouter::getInstance();

    // Создаём очереди
    cmdQ      = xQueueCreate(5, sizeof(uint8_t));
    settingsQ = xQueueCreate(1, sizeof(SettingsPack));
    tripQ     = xQueueCreate(1, sizeof(TripPack));

    // Подписка
    router.subscribe(TOPIC_CMD, cmdQ, QueuePolicy::FIFO_DROP);
    router.subscribe(TOPIC_SETTINGS_PACK, settingsQ, QueuePolicy::OVERWRITE, true);
    router.subscribe(TOPIC_TRIP_PACK, tripQ, QueuePolicy::OVERWRITE, true);

    // Настройка прерываний
    pinMode(INJECTOR_PIN, INPUT_PULLUP);
    pinMode(SHAFT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(INJECTOR_PIN), injectorISR, RISING);
    attachInterrupt(digitalPinToInterrupt(SHAFT_PIN), shaftISR, RISING);

    // Инициализация INA226 (I2C, шунт 0.1 Ом)
    inaReady = ina226.begin(Wire, 0.1f);
    if (inaReady) {
        Serial.println("[RealEngine] INA226 initialized");
    } else {
        Serial.println("[RealEngine] INA226 NOT found, using defaults");
    }

    Serial.printf("[RealEngine] Started (injector=GPIO%d, shaft=GPIO%d)\n",
                  INJECTOR_PIN, SHAFT_PIN);

    unsigned long lastPublish = 0;
    float speedAccum = 0;
    float distanceLast = 0;
    unsigned long speedTime = 0;

    while (1) {
        lastHeartbeat = millis();

        // Обработка очередей
        processCommands();
        processSettingsPack();
        processTripPack();

        // Публикация EnginePack каждые 100 мс
        unsigned long now = millis();
        if (now - lastPublish >= 100) {
            lastPublish = now;

            // Копируем данные из ISR (atomic)
            portDISABLE_INTERRUPTS();
            uint64_t totalUs = injectorTotalUs;
            uint32_t pulses = injectorPulses;
            uint32_t shaftCnt = shaftPulseCount;
            // Сбрасываем накопители для следующего периода
            injectorTotalUs = 0;
            injectorPulses = 0;
            shaftPulseCount = 0;
            portENABLE_INTERRUPTS();

            // --- Расчёт скорости и расстояния ---
            float distanceMeters = (float)shaftCnt / pulsesPerMeter;  // метры за период
            distanceAccum += distanceMeters / 1000.0f;                 // км

            float dtSec = (now - lastPublish) / 1000.0f;
            float speedMs = distanceMeters / dtSec;                   // м/с
            float speedKmh = speedMs * 3.6f;                          // км/ч

            // Сглаживание скорости (скользящее среднее)
            speedAccum = speedAccum * 0.7f + speedKmh * 0.3f;

            // --- Расчёт расхода топлива ---
            float totalSec = totalUs / 1000000.0f;                    // суммарные секунды впрыска
            float fuelMl = totalSec * (injectorFlow * injectorCount / 60.0f);  // мл
            float fuelLiters = fuelMl / 1000.0f;
            fuelUsedAccum += fuelLiters;
            fuelLevel = fuelBase - fuelUsedAccum;

            // --- Расчёт RPM из импульсов форсунки ---
            // Для 4-тактного: 1 впрыск = 2 оборота коленвала на цилиндр
            // RPM = (pulses / period_sec) / injector_count * 60 * 2
            float rpm = 0;
            if (pulses > 0 && dtSec > 0) {
                rpm = ((float)pulses / dtSec / injectorCount) * 60.0f * 2.0f;
            }

            // Двигатель работает если есть впрыск
            bool engRunning = (pulses > 0);

            // --- Напряжение от INA226 (бортсеть) ---
            float voltage = 0;
            if (inaReady) {
                voltage = ina226.readBusVoltage() / 1000.0f;  // мВ → В
            } else {
                voltage = 12.7f;  // заглушка
            }

            // --- Уровень топлива от INA226 (поплавок-реостат через шунт) ---
            float fuelSensorPct = 0;
            if (inaReady) {
                float shuntMV = abs(ina226.readShuntVoltage());
                float range = fuelShuntFull - fuelShuntEmpty;
                if (range > 0) {
                    fuelSensorPct = (shuntMV - fuelShuntEmpty) / range * 100.0f;
                }
                fuelSensorPct = constrain(fuelSensorPct, 0.0f, 100.0f);
            }

            // --- Мгновенный расход ---
            float instantFuel = 0;
            if (speedKmh > 5.0f) {
                instantFuel = (fuelLiters / (distanceMeters / 1000.0f)) * 100.0f;  // л/100км
            } else {
                instantFuel = fuelLiters / dtSec * 3600.0f;  // л/ч на холостых
            }

            // --- Формируем EnginePack ---
            EnginePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version           = 3;
            pack.speed             = speedAccum;
            pack.rpm               = rpm;
            pack.voltage           = voltage;
            pack.engine_running    = engRunning;
            pack.parking_lights    = false;  // TODO: кнопка GPIO27
            pack.instant_fuel      = instantFuel;
            pack.distance          = distanceAccum;
            pack.fuel_used         = fuelUsedAccum;
            pack.fuel_level_sensor = fuelLevel;
            pack.not_fuel          = false;  // TODO: ADC датчика топлива

            router.publishPacket(TOPIC_ENGINE_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void realEngineStart() {
    if (!taskHandle) {
        xTaskCreatePinnedToCore(realEngineTask, "RealEng", TASK_STACK_SIZE, NULL,
                                TASK_PRIORITY_SIMULATOR, &taskHandle, 0);
    }
}

void realEngineStop() {
    if (taskHandle) {
        detachInterrupt(digitalPinToInterrupt(INJECTOR_PIN));
        detachInterrupt(digitalPinToInterrupt(SHAFT_PIN));
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
    }
}

bool realEngineIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 3000; }
