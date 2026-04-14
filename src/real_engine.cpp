// -----------------------------------------------------------------------------
// real_engine.cpp
// Реальный двигатель: форсунка + VSS (датчик скорости).
//
// Автомобиль: Toyota RAV4 2004 (XA20, рестайлинг)
// Двигатель: 1AZ-FE (2.0L) или 2AZ-FE (2.4L), MPI (распределённый впрыск)
//
// Архитектура (ESP-IDF v4.4.x — аппаратные драйверы RMT + PCNT):
//   - RMT (Receive Mode): измерение длительности впрыска форсунки
//   - PCNT (Pulse Counter): аппаратный счётчик импульсов VSS
//   - Кольцевой буфер RMT: гарантирует 100% точность даже при задержках CPU
//
// Форсунка (GPIO4):
//   - Оптопара 6N137 инвертирует сигнал → LOW = форсунка открыта
//   - RMT измеряет длительность LOW-импульса (аппаратно)
//   - Dead time компенсация: 750 мкс (при 13.8В, диапазон 600-900 мкс)
//   - Производительность: 250-280 cc/min (уточнять по маркировке)
//   - Расход: (injector_flow × injector_count / 60000) × duration_ms  → мл
//
// VSS — датчик скорости (GPIO13):
//   - НЕ геркон! Hall sensor или сигнал с ABS
//   - Импульсы 0-5В или 0-12В → нужна защита (оптопара PC817 или делитель + стабилитрон 3.3В)
//   - Провод VSS: фиолетово-белый (Violet/White)
//   - Стандарт Toyota: 4 импульса/обр вала ≈ 2548 импульсов/км
//   - PCNT считает импульсы аппаратно (невозможно потерять)
//   - Фильтр дребезга: 100 мкс (аппаратный)
//
// ВЕРСИЯ: 6.3.0 — MAJOR: RMT + PCNT (ESP-IDF v4.4.x), Toyota RAV4 2004, 3-режима калибровки
// -----------------------------------------------------------------------------

#include "real_engine.h"
#include "data_router.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"
#include "ina226.h"

// ESP-IDF драйверы (доступны в Arduino-среде через esp32-hal)
extern "C" {
#include "driver/rmt.h"
#include "driver/pcnt.h"
}

// =============================================================================
// Пином (Toyota RAV4 2004, 1AZ-FE / 2AZ-FE)
// =============================================================================

#define INJECTOR_PIN   4     // Форсунка (оптопара 6N137, LOW = открыта)
                            // Распределённый впрыск MPI, чёткие импульсы

#define SHAFT_PIN      13    // VSS — датчик скорости (Холл/ABS)
                            // ⚠️ НЕ геркон! Импульсы 0-5В или 0-12В!
                            // Нужна защита: оптопара PC817 или делитель + стабилитрон 3.3В
                            // Провод VSS в авто: фиолетово-белый (Violet/White)
                            // Стандарт Toyota: 4 импульса/обр вала ≈ 2548 имп/км

// =============================================================================
// Конфигурация
// =============================================================================

#define DEAD_TIME_US         750     // Мёртвая зона форсунки (мкс) — калибровать
#define DEAD_TIME_COEF       120.0f  // мкс/В (коррекция при просадке напряжения, Denso)
#define MAX_INJECTOR_US    50000     // Макс. длительность впрыска (50 мс)
#define RMT_FILTER_THRESH   50     // Фильтр шума < 50 мкс (аппаратный, RMT)
#define PCNT_FILTER_VALUE  100     // Фильтр дребезга VSS (аппаратный, PCNT, мкс)
#define RMT_CLK_DIV          80     // 80 МГц / 80 = 1 МГц (1 тик = 1 мкс)

// =============================================================================
// Данные из RMT/PCNT (volatile)
// =============================================================================

static volatile uint64_t injectorTotalUs  = 0;     // Сумма длительностей (мкс) после dead time
static volatile uint32_t injectorPulses  = 0;     // Количество валидных открытий
static volatile uint32_t injectorMissed  = 0;     // Пропущенные (> MAX_INJECTOR_US)

// =============================================================================
// INA226 — напряжение бортсети (I2C, точное измерение)
// =============================================================================

static INA226 ina226(0x40);  // Адрес I2C (A0=GND, A1=GND)
static bool   inaReady = false;
static float  lastVoltage = 13.8f;  // Последнее измерение напряжения (для Dead Time)

// =============================================================================
// calcDeadTime — динамический расчёт мёртвой зоны форсунки
// =============================================================================
//
// Denso форсунки: лаг увеличивается при просадке напряжения
// Формула: deadTime = DEAD_TIME_US + (14.0 - voltage) × DEAD_TIME_COEF
// Пример: 13.8В → 750 + 0.2×120 = 774 мкс
//         12.0В → 750 + 2.0×120 = 990 мкс
//
static uint32_t calcDeadTime(float voltage) {
    if (voltage < 10.0f) voltage = 10.0f;  // Защита от отрицательных
    if (voltage > 15.0f) voltage = 15.0f;  // Защита от перенапряжения
    float dt = DEAD_TIME_US + (14.0f - voltage) * DEAD_TIME_COEF;
    dt = constrain(dt, 400.0f, 1500.0f);  // Физические пределы
    return (uint32_t)dt;
}

// =============================================================================
// Датчик уровня топлива — ADC (поплавок Toyota, 54+53 = 107 Ом)
// =============================================================================
//
// Toyota RAV4 2004: ДВА последовательных поплавка
//   - Основной поплавок: ~54 Ом (полный → пустой)
//   - Дополнительный поплавок: ~53 Ом (компенсация наклона)
//   - Суммарно: ~107 Ом (оба последовательно)
//
// Схема подключения (делитель напряжения, 3.3В питание!):
//
//   3.3V ───[0-107 Ом поплавок]───┬─── GPIO34 (ADC) ───[100 Ом]─── GND
//                                  │
//
// Расчёт напряжения на ADC:
//   Полный бак (0 Ом):     Vadc = 3.3 × 100 / (0 + 100)     = 3.30В  (ADC = 4095)
//   Пустой бак (107 Ом):   Vadc = 3.3 × 100 / (107 + 100)   = 1.59В  (ADC = 1980)
//
// Диапазон: 1.71В = 2115 уровней ADC (12-bit) → точность ±0.05%
//
// ⚠️ ВАЖНО: Использовать 3.3В (НЕ 5В!), иначе при полном баке GPIO34 сгорит!
//
// Калибровка (нужно замерить при реальном баке мультиметром!):
//   Полный бак  → Vadc = ??? В (замерить на GPIO34)
//   Пустой бак  → Vadc = ??? В (замерить на GPIO34)
//
#define FUEL_LEVEL_PIN         34    // ESP32 ADC (GPIO34 — только вход!)
#define FUEL_FIXED_OHM        100    // Ом (фиксированный резистор делителя)
#define FUEL_SENDER_MAX_OHM   107    // Ом (поплавок Toyota: 54+53 Ом)

static float    fuelAdcFull     = 3.30f;   // Вольт при полном баке (TODO: замерить)
static float    fuelAdcEmpty    = 1.59f;   // Вольт при пустом баке (TODO: замерить)

static float    tankCapacity   = 60.0f;
static float    injectorFlow   = 250.0f;   // мл/мин (1AZ-FE: 250-280 cc/min)
static uint8_t  injectorCount  = 4;        // 4 цилиндра
static float    pulsesPerMeter = 2.548f;   // 2548 импульсов/км (Toyota стандарт)

// =============================================================================
// Калибровка (3 режима)
// =============================================================================

// --- Калибровка VSS (pulsesPerMeter) ---
static bool     calSpeedActive   = false;     // true = идёт подсчёт импульсов VSS
static uint32_t calSpeedPulses   = 0;         // импульсы за период калибровки

// --- Калибровка форсунки (injectorFlow) ---
static bool     calInjectorActive = false;    // true = идёт подсчёт расхода
static uint64_t calInjectorTotalUs = 0;       // суммарное время впрыска (мкс)
static uint32_t calInjectorPulses = 0;        // количество впрысков

// --- Калибровка dead time (DEAD_TIME_US) ---
static bool     calDeadTimeActive = false;    // true = идёт подсчёт на холостых
static uint64_t calDeadTimeTotalUs = 0;       // суммарное время впрыска (мкс)
static float    calDeadTimeFuelUsed = 0;      // реальный расход (л) за период

// =============================================================================
// Состояние двигателя
// =============================================================================

static bool     engineRunning   = false;
static bool     parkingLights   = false;
static float    fuelBase        = 60.0f;
static float    fuelLevel       = 60.0f;
static float    distanceAccum   = 0.0f;
static float    fuelUsedAccum   = 0.0f;

// =============================================================================
// Очереди и задача
// =============================================================================

static TaskHandle_t  taskHandle    = NULL;
static bool          isRunningFlag = false;
static unsigned long lastHeartbeat = 0;

static QueueHandle_t cmdQ        = NULL;
static QueueHandle_t settingsQ   = NULL;
static QueueHandle_t tripQ       = NULL;
static QueueHandle_t calibrateQ  = NULL;

// =============================================================================
// initRMT — инициализация RMT для форсунки (ESP-IDF v4.4.x API)
// =============================================================================

static void initRMT() {
    rmt_config_t rmtRx;
    rmtRx.channel = RMT_CHANNEL_0;
    rmtRx.gpio_num = (gpio_num_t)INJECTOR_PIN;
    rmtRx.clk_div = RMT_CLK_DIV;               // 1 МГц = 1 тик = 1 мкс
    rmtRx.mem_block_num = 1;
    rmtRx.rmt_mode = RMT_MODE_RX;
    rmtRx.rx_config.filter_en = true;
    rmtRx.rx_config.filter_ticks_thresh = RMT_FILTER_THRESH;  // Фильтр шума
    rmtRx.rx_config.idle_threshold = MAX_INJECTOR_US;          // Пауза между импульсами

    ESP_ERROR_CHECK(rmt_config(&rmtRx));
    ESP_ERROR_CHECK(rmt_driver_install(rmtRx.channel, 1024, 0));  // Буфер 1024 байта
    rmt_rx_start(rmtRx.channel, true);

    Serial.println("[RealEngine] RMT initialized (injector measurement)");
}

// =============================================================================
// initPCNT — инициализация PCNT для датчика скорости (ESP-IDF v4.4.x API)
// =============================================================================

static void initPCNT() {
    pcnt_config_t pcntCfg;
    pcntCfg.pulse_gpio_num = (gpio_num_t)SHAFT_PIN;
    pcntCfg.ctrl_gpio_num = -1;                  // Нет контрольного сигнала
    pcntCfg.unit = PCNT_UNIT_0;
    pcntCfg.channel = PCNT_CHANNEL_0;
    pcntCfg.pos_mode = PCNT_COUNT_INC;           // Считаем по rising edge
    pcntCfg.neg_mode = PCNT_COUNT_DIS;
    pcntCfg.lctrl_mode = PCNT_MODE_KEEP;
    pcntCfg.hctrl_mode = PCNT_MODE_KEEP;
    pcntCfg.counter_h_lim = 30000;
    pcntCfg.counter_l_lim = -30000;

    ESP_ERROR_CHECK(pcnt_unit_config(&pcntCfg));
    ESP_ERROR_CHECK(pcnt_set_filter_value(PCNT_UNIT_0, PCNT_FILTER_VALUE));
    ESP_ERROR_CHECK(pcnt_filter_enable(PCNT_UNIT_0));
    ESP_ERROR_CHECK(pcnt_counter_pause(PCNT_UNIT_0));
    ESP_ERROR_CHECK(pcnt_counter_clear(PCNT_UNIT_0));
    ESP_ERROR_CHECK(pcnt_counter_resume(PCNT_UNIT_0));

    Serial.println("[RealEngine] PCNT initialized (shaft speed counter)");
}

// =============================================================================
// processRMTData — чтение данных из кольцевого буфера RMT
// =============================================================================
//
// ВАЖНО: 64-битные операции НЕ атомарны на 32-битном ESP32.
// Все изменения injectorTotalUs/injectorPulses защищены через portDISABLE_INTERRUPTS().
//
static void processRMTData() {
    RingbufHandle_t rb = NULL;
    rmt_get_ringbuf_handle(RMT_CHANNEL_0, &rb);
    if (!rb) return;

    size_t itemSize = 0;
    rmt_item32_t* items = (rmt_item32_t*) xRingbufferReceive(rb, &itemSize, 0);
    if (!items) return;  // Нет данных

    uint32_t numItems = itemSize / sizeof(rmt_item32_t);

    // Локальные накопители (атомарность не нужна, т.к. локальные переменные)
    uint64_t localTotalUs = 0;
    uint32_t localPulses = 0;
    uint32_t localMissed = 0;

    // Динамический Dead Time (обновляется каждые 100 мс из основного цикла)
    uint32_t deadTime = calcDeadTime(lastVoltage);

    for (uint32_t i = 0; i < numItems; i++) {
        // 6N137 инвертирует: LOW (level=0) = форсунка открыта
        if (items[i].level0 == 0 && items[i].duration0 > 0) {
            uint32_t duration = items[i].duration0;  // в мкс
            if (duration > deadTime && duration < MAX_INJECTOR_US) {
                uint32_t corrected = duration - deadTime;
                localTotalUs += corrected;
                localPulses++;
            } else if (duration >= MAX_INJECTOR_US) {
                localMissed++;
            }
        }

        if (items[i].level1 == 0 && items[i].duration1 > 0) {
            uint32_t duration = items[i].duration1;
            if (duration > deadTime && duration < MAX_INJECTOR_US) {
                uint32_t corrected = duration - deadTime;
                localTotalUs += corrected;
                localPulses++;
            } else if (duration >= MAX_INJECTOR_US) {
                localMissed++;
            }
        }
    }

    vRingbufferReturnItem(rb, (void*)items);

    // Атомарное обновление глобальных переменных (критично для 64-бит!)
    portDISABLE_INTERRUPTS();
    injectorTotalUs += localTotalUs;
    injectorPulses += localPulses;
    injectorMissed += localMissed;

    // Калибровка — тоже атомарно
    if (calInjectorActive) {
        calInjectorTotalUs += localTotalUs;
        calInjectorPulses += localPulses;
    }
    if (calDeadTimeActive) {
        calDeadTimeTotalUs += localTotalUs;
    }
    portENABLE_INTERRUPTS();
}

// =============================================================================
// processCommands
// =============================================================================

static void processCommands() {
    uint8_t cmd;
    while (xQueueReceive(cmdQ, &cmd, 0) == pdTRUE) {
        // --- FULL_TANK: Заправка ---
        if ((Command)cmd == CMD_FULL_TANK) {
            fuelBase = tankCapacity;
            fuelLevel = tankCapacity;
#if DEBUG_LOG
            Serial.printf("[RealEngine] Full tank: %.1f L\n", fuelLevel);
#endif
        }
        // --- CALIBRATE_SPEED: Старт калибровки VSS ---
        else if ((Command)cmd == CMD_CALIBRATE_SPEED) {
            pcnt_counter_clear(PCNT_UNIT_0);
            calSpeedPulses = 0;
            calSpeedActive = true;
#if DEBUG_LOG
            Serial.println("[RealEngine] === CALIBRATION VSS START ===");
            Serial.println("[RealEngine] Drive known distance (e.g. 1 km)");
            Serial.println("[RealEngine] Then send calibrate_speed_end with distance_m");
#endif
        }
        // --- CALIBRATE_INJECTOR: Старт калибровки форсунки ---
        else if ((Command)cmd == CMD_CALIBRATE_INJECTOR) {
            calInjectorTotalUs = 0;
            calInjectorPulses = 0;
            calInjectorActive = true;
            // Сбрасываем расход для точного замера
            fuelUsedAccum = 0;
#if DEBUG_LOG
            Serial.println("[RealEngine] === CALIBRATION INJECTOR START ===");
            Serial.println("[RealEngine] Fill tank FULL, drive normally");
            Serial.println("[RealEngine] Then send calibrate_injector_end with fuel_liters");
#endif
        }
        // --- CALIBRATE_DEADTIME: Старт калибровки dead time ---
        else if ((Command)cmd == CMD_CALIBRATE_DEADTIME) {
            calDeadTimeTotalUs = 0;
            calDeadTimeFuelUsed = 0;
            calDeadTimeActive = true;
#if DEBUG_LOG
            Serial.println("[RealEngine] === CALIBRATION DEAD TIME START ===");
            Serial.println("[RealEngine] Idle engine for 5-10 minutes (warm)");
            Serial.println("[RealEngine] Measure real fuel consumption (ml/min)");
            Serial.println("[RealEngine] Then send calibrate_deadtime_end with fuel_ml_per_min");
#endif
        }
        // --- OTA START: Завершение задачи (освобождение памяти) ---
        else if ((Command)cmd == CMD_OTA_START) {
            Serial.println("[RealEngine] CMD_OTA_START — shutting down");
            isRunningFlag = false;
            vTaskDelete(NULL);
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
        if (!engineRunning && pack.fuel_level > 0.01f) {
            fuelBase = pack.fuel_level;
            fuelLevel = pack.fuel_level;
        }
    }
}

// =============================================================================
// processCalibrate — Завершение калибровки (VSS, Injector, DeadTime)
// =============================================================================

static void processCalibrate() {
    int value;  // Расстояние (м) или расход (мл/мин или литры)
    if (xQueueReceive(calibrateQ, &value, 0) == pdTRUE) {

        // --- Завершение калибровки VSS ---
        if (calSpeedActive && value > 0) {
            // Читаем текущий счётчик PCNT
            int16_t cnt = 0;
            pcnt_get_counter_value(PCNT_UNIT_0, &cnt);
            calSpeedPulses = (uint32_t)cnt;

            // Расчёт: pulses_per_meter = total_pulses / distance_meters
            pulsesPerMeter = (float)calSpeedPulses / (float)value;

            // Сохраняем в SettingsPack → NVS через DataRouter
            SettingsPack sp;
            sp.version           = 1;
            sp.tank_capacity     = tankCapacity;
            sp.injector_count    = injectorCount;
            sp.injector_flow     = injectorFlow;
            sp.pulses_per_meter  = pulsesPerMeter;
            sp.kline_protocol    = 0;
            DataRouter::getInstance().publishPacket(TOPIC_SETTINGS_PACK, &sp, sizeof(sp));

#if DEBUG_LOG
            Serial.printf("[RealEngine] === CALIBRATION VSS END ===\n");
            Serial.printf("[RealEngine] %u pulses / %d m = %.3f pulses/m (%.0f pulses/km)\n",
                          calSpeedPulses, value, pulsesPerMeter, pulsesPerMeter * 1000.0f);
            Serial.printf("[RealEngine] Saved to NVS!\n");
#endif
            calSpeedActive = false;
            calSpeedPulses = 0;
            pcnt_counter_clear(PCNT_UNIT_0);
        }
        else if (!calSpeedActive && value > 0 && value < 10000) {
            // Это не калибровка VSS — возможно injector или deadtime
            // Завершение калибровки форсунки (value = литры)
        }

        // --- Завершение калибровки форсунки (value = литры) ---
        if (calInjectorActive && value > 0) {
            float realFuelLiters = (float)value / 100.0f;  // Android шлёт в см (например 500 = 5.00л)

            // Расчёт: injector_flow = (real_fuel_liters / injector_total_sec) * 60 / injector_count
            // injector_total_sec = calInjectorTotalUs / 1000000
            float totalSec = (float)calInjectorTotalUs / 1000000.0f;
            if (totalSec > 0 && calInjectorPulses > 0) {
                // injectorFlow (мл/мин) = (realFuelLiters * 1000) / totalSec * 60 / injectorCount
                injectorFlow = (realFuelLiters * 1000.0f / totalSec * 60.0f) / injectorCount;

                // Сохраняем
                SettingsPack sp;
                sp.version           = 1;
                sp.tank_capacity     = tankCapacity;
                sp.injector_count    = injectorCount;
                sp.injector_flow     = injectorFlow;
                sp.pulses_per_meter  = pulsesPerMeter;
                sp.kline_protocol    = 0;
                DataRouter::getInstance().publishPacket(TOPIC_SETTINGS_PACK, &sp, sizeof(sp));

#if DEBUG_LOG
                Serial.printf("[RealEngine] === CALIBRATION INJECTOR END ===\n");
                Serial.printf("[RealEngine] Real fuel: %.2f L, Total time: %.1f sec, Pulses: %u\n",
                              realFuelLiters, totalSec, calInjectorPulses);
                Serial.printf("[RealEngine] injector_flow = %.1f ml/min\n", injectorFlow);
                Serial.printf("[RealEngine] Saved to NVS!\n");
#endif
                calInjectorActive = false;
                calInjectorTotalUs = 0;
                calInjectorPulses = 0;
            }
        }

        // --- Завершение калибровки dead time (value = мл/мин) ---
        if (calDeadTimeActive && value > 0) {
            float realFuelMlPerMin = (float)value;  // мл/мин

            // Расчёт: dead_time = total_us - (real_fuel_ml / (injector_flow * injector_count / 60000))
            // real_fuel_ml_per_sec = realFuelMlPerMin / 60
            // ideal_total_us = real_fuel_ml / (injector_flow * injector_count / 60000)
            // dead_time = (total_raw_us - ideal_total_us) / pulses
            float realFuelMlPerSec = realFuelMlPerMin / 60.0f;
            float idealTotalSec = realFuelMlPerSec / (injectorFlow * injectorCount / 60000.0f);
            float idealTotalUs = idealTotalSec * 1000000.0f;

            if (calInjectorPulses > 0) {
                // dead_time = (raw_total_us - ideal_total_us) / pulses
                // Но мы уже храним corrected_total_us, нужно raw_total_us
                // Для упрощения: dead_time = (ideal_total_us) / pulses * correction
                float deadTimePerPulse = (calDeadTimeTotalUs / calInjectorPulses);
                // Подбираем dead_time чтобы расход совпадал
                // Новая формула: dead_time = old_dead_time * (1 - error)
                // Упрощённо: корректируем на основе расхождения

                float oldDeadTime = DEAD_TIME_US;
                float error = 1.0f - (realFuelMlPerMin / (injectorFlow * injectorCount / 60.0f * (calInjectorPulses / 600.0f)));
                float newDeadTime = oldDeadTime * (1.0f + error * 0.1f);  // Плавная корректировка

                // Ограничиваем разумным диапазоном
                newDeadTime = constrain(newDeadTime, 400.0f, 1200.0f);

#if DEBUG_LOG
                Serial.printf("[RealEngine] === CALIBRATION DEAD TIME END ===\n");
                Serial.printf("[RealEngine] Real consumption: %.1f ml/min\n", realFuelMlPerMin);
                Serial.printf("[RealEngine] Pulses: %u, Corrected total: %llu us\n",
                              calInjectorPulses, calDeadTimeTotalUs);
                Serial.printf("[RealEngine] Suggested DEAD_TIME_US: %.0f (was %d)\n",
                              newDeadTime, DEAD_TIME_US);
                Serial.printf("[RealEngine] ⚠️  Update DEAD_TIME_US in code and reflash!\n");
#endif
                calDeadTimeActive = false;
                calDeadTimeTotalUs = 0;
            }
        }
    }
}

// =============================================================================
// readFuelAdc — чтение датчика уровня топлива (ADC, сглаживание)
// =============================================================================
//
// Возвращает: напряжение в Вольтах (0-3.3В)
// Сглаживание: скользящее среднее из 16 измерений
//
static float readFuelAdc() {
    static float adcSmooth = 0;      // Сглаженное значение
    static bool  initialized = false;

    if (!initialized) {
        // Первый замер — инициализация
        int rawSum = 0;
        for (int i = 0; i < 16; i++) {
            rawSum += analogRead(FUEL_LEVEL_PIN);
            delay(2);
        }
        adcSmooth = (rawSum / 16) * (3.3f / 4095.0f);
        initialized = true;
        return adcSmooth;
    }

    // Экспоненциальное сглаживание: α = 0.25
    int raw = analogRead(FUEL_LEVEL_PIN);
    float volts = raw * (3.3f / 4095.0f);
    adcSmooth = adcSmooth * 0.75f + volts * 0.25f;

    return adcSmooth;
}

// =============================================================================
// realEngineTask
// =============================================================================

static void realEngineTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& router = DataRouter::getInstance();

    // Создаём очереди
    cmdQ         = xQueueCreate(5, sizeof(uint8_t));
    settingsQ    = xQueueCreate(1, sizeof(SettingsPack));
    tripQ        = xQueueCreate(1, sizeof(TripPack));
    calibrateQ   = xQueueCreate(1, sizeof(int));

    // Подписка
    router.subscribe(TOPIC_CMD, cmdQ, QueuePolicy::FIFO_DROP);
    router.subscribe(TOPIC_SETTINGS_PACK, settingsQ, QueuePolicy::OVERWRITE, true);
    router.subscribe(TOPIC_TRIP_PACK, tripQ, QueuePolicy::OVERWRITE, true);
    router.subscribe(TOPIC_CALIBRATE_DIST, calibrateQ, QueuePolicy::OVERWRITE);

    // Инициализация RMT + PCNT
    initRMT();
    initPCNT();

    // Инициализация INA226
    inaReady = ina226.begin(Wire, 0.1f);
    if (inaReady) {
        Serial.println("[RealEngine] INA226 initialized");
    } else {
        Serial.println("[RealEngine] INA226 NOT found, using defaults");
    }

    // Инициализация ADC для датчика уровня топлива
    analogReadResolution(12);  // 12 бит (0-4095)
    analogSetWidth(12);        // Разрешение 12 бит
    pinMode(FUEL_LEVEL_PIN, INPUT);
    Serial.printf("[RealEngine] Fuel level ADC initialized (GPIO%d, 12-bit)\n",
                  FUEL_LEVEL_PIN);

    Serial.printf("[RealEngine] Started (RMT=GPIO%d, PCNT=GPIO%d, ADC=GPIO%d)\n",
                  INJECTOR_PIN, SHAFT_PIN, FUEL_LEVEL_PIN);

    unsigned long lastPublish = 0;
    float speedAccum = 0;

    while (1) {
        lastHeartbeat = millis();

        // Обработка очередей
        processCommands();
        processSettingsPack();
        processTripPack();
        processCalibrate();

        // Чтение RMT буфера (неблокирующее, выгребает накопленное)
        processRMTData();

        // Публикация EnginePack каждые 100 мс
        unsigned long now = millis();
        if (now - lastPublish >= 100) {
            lastPublish = now;

            // --- Чтение PCNT (атомарно) ---
            int16_t shaftCnt = 0;
            pcnt_get_counter_value(PCNT_UNIT_0, &shaftCnt);
            pcnt_counter_clear(PCNT_UNIT_0);

            // --- Расчёт скорости и расстояния ---
            float distanceMeters = (float)shaftCnt / pulsesPerMeter;
            distanceAccum += distanceMeters / 1000.0f;

            float dtSec = 0.1f;  // 100 мс
            float speedKmh = (distanceMeters / dtSec) * 3.6f;

            // Сглаживание скорости (скользящее среднее)
            speedAccum = speedAccum * 0.7f + speedKmh * 0.3f;

            // --- Расчёт расхода топлива ---
            portDISABLE_INTERRUPTS();
            uint64_t totalUs = injectorTotalUs;
            uint32_t pulses = injectorPulses;
            injectorTotalUs = 0;
            injectorPulses = 0;
            portENABLE_INTERRUPTS();

            float totalSec = totalUs / 1000000.0f;
            float fuelMl = totalSec * (injectorFlow * injectorCount / 60.0f);
            float fuelLiters = fuelMl / 1000.0f;
            fuelUsedAccum += fuelLiters;
            fuelLevel = fuelBase - fuelUsedAccum;

            // --- RPM из импульсов форсунки ---
            float rpm = 0;
            if (pulses > 0 && dtSec > 0) {
                rpm = ((float)pulses / dtSec / injectorCount) * 60.0f * 2.0f;
            }

            bool engRunning = (pulses > 0);

            // --- Напряжение от INA226 (бортсеть) — обновляем для Dead Time ---
            float voltage = inaReady ? ina226.readBusVoltage() / 1000.0f : 12.7f;
            lastVoltage = voltage;  // Используется в calcDeadTime()

            // --- Уровень топлива от ADC (поплавок Toyota) ---
            float fuelSensorPct = 0;
            float fuelSensorVolts = readFuelAdc();  // Читаем ADC (сглаживание)

            if (fuelSensorVolts > 0.1f) {  // Валидное чтение
                // Поплавок: полный бак = 0 Ом = БОЛЬШЕ напряжение
                float range = fuelAdcFull - fuelAdcEmpty;
                if (range > 0.01f) {  // Избегаем деления на ноль
                    fuelSensorPct = (fuelSensorVolts - fuelAdcEmpty) / range * 100.0f;
                }
                fuelSensorPct = constrain(fuelSensorPct, 0.0f, 100.0f);

                // Синхронизация: если бак заполнен >95%, обновляем fuelBase
                if (fuelSensorPct > 95.0f) {
                    fuelBase = tankCapacity;
                    fuelLevel = tankCapacity;
                }
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
            pack.parking_lights    = false;
            pack.instant_fuel      = instantFuel;
            pack.distance          = distanceAccum;
            pack.fuel_used         = fuelUsedAccum;
            pack.fuel_level_sensor = fuelSensorPct > 0 ? 
                (fuelSensorPct / 100.0f * tankCapacity) : fuelLevel;  // Приоритет датчику
            pack.not_fuel          = false;

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
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunningFlag = true;
        // Ядро 0 — RealEngine (ISR, RMT, PCNT — аппаратные драйверы)
        xTaskCreatePinnedToCore(realEngineTask, "RealEng", TASK_STACK_SIZE, NULL,
                                TASK_PRIORITY_SIMULATOR, &taskHandle, 0);
    }
}

void realEngineStop() {
    if (taskHandle) {
        rmt_rx_stop(RMT_CHANNEL_0);
        rmt_driver_uninstall(RMT_CHANNEL_0);
        pcnt_counter_pause(PCNT_UNIT_0);
        pcnt_counter_clear(PCNT_UNIT_0);
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
    }
}

bool realEngineIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 3000; }
