// -----------------------------------------------------------------------------
// oled_task.cpp — OLED SSD1306 128x64 (I2C)
// Обёрнуто в OLED_ENABLED: при сборке без дисплея код исключается.

#include "oled_task.h"

#if OLED_ENABLED

#include "data_router.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "icons.h"
#include "app_config.h"
#include <U8g2lib.h>
#include <Wire.h>

#define OLED_SDA 21
#define OLED_SCL 22

static TaskHandle_t  oledTaskHandle = NULL;
static bool          isRunning      = false;
static unsigned long lastHeartbeat  = 0;
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Данные для отображения
static float displaySpeed = 0, displayRpm = 0, displayFuel = 0;
static float displayVoltage = 12.7f, displayConsumption = 0;
static bool  displayEngine = false, displayBtConnected = false;
static bool  displayParkingLights = false;
static float tankCapacity = 60.0f;

// =============================================================================
// Вспомогательные функции
// =============================================================================

static void drawProgressBar(int x, int y, int w, int h, float pct) {
    pct = constrain(pct, 0.0f, 1.0f);
    u8g2.drawFrame(x, y, w, h);
    int fw = (int)(w * pct);
    if (fw > 0) u8g2.drawBox(x, y, fw, h);
}

static void drawBtIcon(int x, int y, bool on) {
    u8g2.drawBitmap(x, y, 2, 16, on ? ic_bt_connected : ic_bt_disconnected);
}

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
// oledTask — Главная задача
// =============================================================================

void oledTask(void* parameter) {
    (void)parameter;
    isRunning = true;

    Wire.begin(OLED_SDA, OLED_SCL);
    delay(10);
    u8g2.begin();

    // Приветственный экран
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(0, 10); u8g2.print("Car BKV2");
    u8g2.setCursor(0, 25); u8g2.print("ESP32 + OLED");
    u8g2.setCursor(0, 40); u8g2.print("Pub/Sub Arch");
    u8g2.sendBuffer();
    lastHeartbeat = millis();
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Подписки через DataRouter (модуль создаёт очередь → subscribe)
    DataRouter& dr = DataRouter::getInstance();

    QueueHandle_t engineQ = xQueueCreate(1, sizeof(EnginePack));
    QueueHandle_t tripQ   = xQueueCreate(1, sizeof(TripPack));
    QueueHandle_t btQ     = xQueueCreate(1, sizeof(bool));

    dr.subscribe(TOPIC_ENGINE_PACK,    engineQ, QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRIP_PACK,      tripQ,   QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRANSPORT_STATUS, btQ,   QueuePolicy::OVERWRITE, true);  // retain

    // Подписка на команды (только для CMD_OTA_START)
    QueueHandle_t cmdQ = xQueueCreate(1, sizeof(uint8_t));
    dr.subscribe(TOPIC_CMD, cmdQ, QueuePolicy::FIFO_DROP);

    // Начальный BT статус из кэша
    bool btState;
    if (dr.getCached(TOPIC_TRANSPORT_STATUS, btState)) {
        displayBtConnected = btState;
    }

#if DEBUG_LOG
    Serial.println("[OLED] Ready (DataRouter-based)");
#endif

    unsigned long lastUpdate = 0;

    while (1) {
        lastHeartbeat = millis();

        // Проверка CMD_OTA_START
        if (cmdQ) {
            uint8_t cmd;
            while (xQueueReceive(cmdQ, &cmd, 0) == pdTRUE) {
                if (cmd == CMD_OTA_START) {
                    Serial.println("[OLED] CMD_OTA_START — shutting down");
                    isRunning = false;
                    vTaskDelete(NULL);
                }
            }
        }

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

        // Обновление дисплея каждые 200 мс
        unsigned long now = millis();
        if (now - lastUpdate >= 200) {
            lastUpdate = now;
            oledUpdate();
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

void oledStart() {
    if (!oledTaskHandle) {
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunning = true;
        // Ядро 0 — OLED (дисплей)
        xTaskCreatePinnedToCore(oledTask, "OLED", TASK_STACK_SIZE, NULL, 2, &oledTaskHandle, 0);
#if DEBUG_LOG
        Serial.println("[OLED] Started (8K stack, P2)");
#endif
    }
}

void oledStop() {
    if (oledTaskHandle) {
        vTaskDelete(oledTaskHandle);
        oledTaskHandle = NULL;
        isRunning = false;
    }
}

bool oledIsRunning() { return isRunning; }

#endif // OLED_ENABLED


