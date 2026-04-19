// -----------------------------------------------------------------------------
// oled_task.cpp — OLED SSD1306 128x64 (I2C)
// Обёрнуто в OLED_ENABLED: при сборке без дисплея код исключается.
//
// Назначение:
// - Подписывается на топики DataRouter: EnginePack, TripPack, TransportStatus
// - Обновляет дисплей каждые 200 мс (5 Гц)
// - Отображает: статус двигателя, скорость, RPM, топливо + прогресс-бар, расход
// - Индикатор Bluetooth и габаритов в правом верхнем углу
//
// Аппаратная конфигурация:
// - SDA = GPIO 21, SCL = GPIO 22
// - Дисплей: SSD1306 128x64, аппаратный I2C (U8g2)
//
// Обновление (интеграция с task_common):
// - Использует taskInit() для инициализации и подписки на TOPIC_CMD
// - Регистрирует подписки через taskRegisterSubscription()
// - taskProcessCommands() обрабатывает CMD_OTA_START с полной очисткой ресурсов
// - taskHeartbeat() обновляет счётчик для loop()-мониторинга
// - При OTA освобождаются I2C и дисплей, очереди удаляются
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include "oled_task.h"

#if OLED_ENABLED

#include "data_router.h"
#include "task_common.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "icons.h"
#include "app_config.h"
#include "debug.h"
#include <U8g2lib.h>
#include <Wire.h>

// =============================================================================
// Пины I2C для OLED
// =============================================================================
#define OLED_SDA 21
#define OLED_SCL 22

// =============================================================================
// Глобальные переменные модуля
// =============================================================================

static TaskHandle_t  oledTaskHandle = NULL;
static bool          isRunning      = false;
static unsigned long lastHeartbeat  = 0;

// --- Контекст задачи (фреймворк task_common) ---
// Внедрение фреймворка task_common:
//   - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
//   - Единый heartbeat для мониторинга в loop()
//   - Регистрация подписок для корректной отписки при завершении
static TaskContext ctx = {0};

// --- Очереди для подписки на данные ---
// Создаются модулем, регистрируются через taskRegisterSubscription()
static QueueHandle_t engineQ = NULL;
static QueueHandle_t tripQ   = NULL;
static QueueHandle_t btQ     = NULL;

// --- Дисплей ---
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// --- Данные для отображения (кэш) ---
static float displaySpeed = 0;
static float displayRpm = 0;
static float displayFuel = 0;
static float displayVoltage = 12.7f;
static float displayConsumption = 0;
static bool  displayEngine = false;
static bool  displayBtConnected = false;
static bool  displayParkingLights = false;
static float tankCapacity = 60.0f;

// =============================================================================
// Вспомогательные функции отрисовки
// =============================================================================

// drawProgressBar — отрисовка прогресс-бара (уровень топлива)
static void drawProgressBar(int x, int y, int w, int h, float pct) {
    pct = constrain(pct, 0.0f, 1.0f);
    u8g2.drawFrame(x, y, w, h);
    int fw = (int)(w * pct);
    if (fw > 0) u8g2.drawBox(x, y, fw, h);
}

// drawBtIcon — отрисовка иконки Bluetooth (подключен/отключен)
static void drawBtIcon(int x, int y, bool on) {
    u8g2.drawBitmap(x, y, 2, 16, on ? ic_bt_connected : ic_bt_disconnected);
}

// oledUpdate — полная перерисовка экрана
static void oledUpdate() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);

    // Строка 1: статус двигателя
    u8g2.setCursor(0, 10);
    u8g2.print(displayEngine ? "ENG:RUN" : "ENG:OFF");

    // Строка 2: скорость
    u8g2.setCursor(0, 20);
    u8g2.print("SPD:");
    u8g2.print(displaySpeed, 1);
    u8g2.print(" km/h");

    // Строка 3: обороты
    u8g2.setCursor(0, 30);
    u8g2.print("RPM:");
    u8g2.print((int)displayRpm);

    // Строка 4: топливо + прогресс-бар
    u8g2.setCursor(0, 40);
    u8g2.print("FUEL:");
    u8g2.print(displayFuel, 1);
    u8g2.print("L");
    drawProgressBar(75, 34, 50, 5, (tankCapacity > 0) ? (displayFuel / tankCapacity) : 0);

    // Строка 5: средний расход
    u8g2.setCursor(0, 50);
    u8g2.print("AVG:");
    u8g2.print(displayConsumption, 1);
    u8g2.print(" L/100");

    // Иконки в правом верхнем углу: габариты + BT
    if (displayParkingLights) {
        u8g2.drawBitmap(96, 0, 2, 16, ic_parking_lights);
    }
    drawBtIcon(displayParkingLights ? 112 : 96, 0, displayBtConnected);

    u8g2.sendBuffer();
}

// =============================================================================
// oledCmdHandler — обработка специфичных команд модуля OLED
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
static bool oledCmdHandler(uint8_t cmd) {
    switch ((Command)cmd) {
        // OLED не имеет специфичных команд, но обработчик необходим
        // для корректной работы фреймворка
        
        default:
            return false;  // Команда не распознана
    }
}

// =============================================================================
// oledPreShutdownCleanup — освобождение ресурсов дисплея перед OTA
// =============================================================================
// Вызывается из taskProcessCommands() при получении CMD_OTA_START
// ДО вызова taskShutdown().
//
// Освобождает:
//   - I2C (Wire.end())
//   - Дисплей (выключается для экономии энергии)
//
// ВАЖНО: эта функция НЕ должна вызывать taskShutdown или vTaskDelete,
// она только освобождает специфичные для модуля аппаратные ресурсы.
//
static void oledPreShutdownCleanup(void) {
    DBG_PRINTLN("[OLED] Pre-shutdown cleanup: clearing display and releasing I2C...");
    
    // Очищаем экран и выключаем дисплей
    u8g2.clear();
    u8g2.setPowerSave(1);  // Выключаем дисплей для экономии энергии
    u8g2.sendBuffer();
    
    // Освобождаем I2C
    Wire.end();
    
    DBG_PRINTLN("[OLED] Display and I2C released");
}

// =============================================================================
// oledCmdHandlerWithCleanup — расширенный обработчик с очисткой
// =============================================================================
// Обёртка над oledCmdHandler, которая также вызывает очистку дисплея
// при получении CMD_OTA_START.
//
static bool oledCmdHandlerWithCleanup(uint8_t cmd) {
    if ((Command)cmd == CMD_OTA_START) {
        // Освобождаем дисплей и I2C ДО вызова taskShutdown()
        oledPreShutdownCleanup();
        // Возвращаем false — фреймворк продолжит обработку и вызовет taskShutdown()
        return false;
    }
    
    // Для остальных команд — обычный обработчик
    return oledCmdHandler(cmd);
}

// =============================================================================
// oledTask — Главная задача FreeRTOS
// =============================================================================
//
// Архитектура на основе task_common:
//   1. taskInit() — инициализация, создание cmdQ, подписка на TOPIC_CMD
//   2. Инициализация I2C и дисплея
//   3. Создание очередей и подписка на топики данных
//   4. Основной цикл:
//      - taskHeartbeat() — обновление счётчика активности
//      - Чтение очередей данных (EnginePack, TripPack, TransportStatus)
//      - taskProcessCommands() — обработка команд (включая OTA)
//      - Обновление дисплея каждые 200 мс
//
// При получении CMD_OTA_START:
//   - oledCmdHandlerWithCleanup() вызывает oledPreShutdownCleanup()
//   - taskProcessCommands() вызывает taskShutdown()
//   - taskShutdown() отписывается от топиков, удаляет очереди, завершает задачу
//   - Память и дисплей полностью освобождаются для OTA-обновления
//
void oledTask(void* parameter) {
    (void)parameter;
    
    // === ИНИЦИАЛИЗАЦИЯ ЧЕРЕЗ ФРЕЙМВОРК ===
    // taskInit() выполняет:
    //   - Установку isRunningFlag = true
    //   - Создание cmdQ (очередь команд)
    //   - Подписку на TOPIC_CMD
    //   - Регистрацию подписки для автоматической очистки при shutdown
    if (!taskInit(&ctx, "OLED", &isRunning, &lastHeartbeat)) {
        DBG_PRINTLN("[OLED] ERROR: taskInit failed!");
        isRunning = false;
        vTaskDelete(NULL);
        return;
    }

    // === ИНИЦИАЛИЗАЦИЯ ДИСПЛЕЯ ===
    Wire.begin(OLED_SDA, OLED_SCL);
    delay(10);
    u8g2.begin();
    u8g2.setPowerSave(0);  // Включаем дисплей

    // Приветственный экран
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(0, 10); u8g2.print("Car BKV2");
    u8g2.setCursor(0, 25); u8g2.print("ESP32 + OLED");
    u8g2.setCursor(0, 40); u8g2.print("Pub/Sub Arch");
    u8g2.setCursor(0, 55); u8g2.print(FW_VERSION_STR);
    u8g2.sendBuffer();
    
    lastHeartbeat = millis();
    vTaskDelay(500 / portTICK_PERIOD_MS);

    DataRouter& dr = DataRouter::getInstance();

    // === СОЗДАНИЕ ОЧЕРЕДЕЙ И ПОДПИСКА НА ТОПИКИ ===
    engineQ = xQueueCreate(1, sizeof(EnginePack));
    tripQ   = xQueueCreate(1, sizeof(TripPack));
    btQ     = xQueueCreate(1, sizeof(bool));

    if (!engineQ || !tripQ || !btQ) {
        DBG_PRINTLN("[OLED] ERROR: Failed to create queues!");
        isRunning = false;
        vTaskDelete(NULL);
        return;
    }

    dr.subscribe(TOPIC_ENGINE_PACK,      engineQ, QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRIP_PACK,        tripQ,   QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRANSPORT_STATUS, btQ,     QueuePolicy::OVERWRITE, true);  // retain

    // Регистрируем подписки для автоматической отписки при shutdown
    taskRegisterSubscription(&ctx, TOPIC_ENGINE_PACK,      engineQ);
    taskRegisterSubscription(&ctx, TOPIC_TRIP_PACK,        tripQ);
    taskRegisterSubscription(&ctx, TOPIC_TRANSPORT_STATUS, btQ);

    // Начальный BT статус из кэша
    bool btState;
    if (dr.getCached(TOPIC_TRANSPORT_STATUS, btState)) {
        displayBtConnected = btState;
    }

    unsigned long lastUpdate = 0;
    const unsigned long UPDATE_INTERVAL = 200;  // 5 Гц

    while (1) {
        // Heartbeat — обновление счётчика для loop()-мониторинга
        taskHeartbeat(&ctx);

        // Чтение EnginePack
        EnginePack pEng;
        if (engineQ && xQueueReceive(engineQ, &pEng, 0) == pdTRUE) {
            displaySpeed = pEng.speed;
            displayRpm = pEng.rpm;
            displayEngine = pEng.engine_running;
            displayParkingLights = pEng.parking_lights;
        }

        // Чтение TripPack
        TripPack pTrip;
        if (tripQ && xQueueReceive(tripQ, &pTrip, 0) == pdTRUE) {
            displayFuel = pTrip.fuel_level;
            displayConsumption = pTrip.avg_consumption;
        }

        // Чтение BT статуса
        bool btVal;
        if (btQ && xQueueReceive(btQ, &btVal, 0) == pdTRUE) {
            displayBtConnected = btVal;
        }

        // Обработка команд (CMD_OTA_START обрабатывается автоматически)
        // Используем расширенный обработчик с очисткой дисплея
        taskProcessCommands(&ctx, oledCmdHandlerWithCleanup);

        // Обновление дисплея каждые 200 мс
        unsigned long now = millis();
        if (now - lastUpdate >= UPDATE_INTERVAL) {
            lastUpdate = now;
            oledUpdate();
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void oledStart() {
    if (!oledTaskHandle) {
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunning = true;
        // Ядро 1 — OLED (дисплей)
        xTaskCreatePinnedToCore(oledTask, "OLED", TASK_STACK_SIZE, NULL, 
                                TASK_PRIORITY_OLED, &oledTaskHandle, 1);
    }
}

void oledStop() {
    if (oledTaskHandle) {
        // Очищаем дисплей перед удалением задачи
        u8g2.clear();
        u8g2.setPowerSave(1);
        u8g2.sendBuffer();
        Wire.end();
        
        vTaskDelete(oledTaskHandle);
        oledTaskHandle = NULL;
        isRunning = false;
        DBG_PRINTLN("[OLED] Stopped");
    }
}

bool oledIsRunning() { 
    return isRunning; 
}

#endif // OLED_ENABLED