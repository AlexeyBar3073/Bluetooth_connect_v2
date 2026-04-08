// -----------------------------------------------------------------------------
// oled_task.cpp
// Вывод на OLED SSD1306 128x64 (I2C).
//
// Назначение:
// - Подписка на EnginePack (OVERWRITE) → скорость, RPM, статус двигателя
// - Подписка на TripPack (OVERWRITE) → топливо, расход
// - Подписка на TOPIC_TRANSPORT_STATUS → статус Bluetooth
// - Обновление дисплея каждые 200 мс
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура
// -----------------------------------------------------------------------------

#include "oled_task.h"
#include "data_bus.h"
#include "topics.h"
#include "packets.h"
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

    u8g2.setCursor(0, 10);
    u8g2.print(displayBtConnected ? "BT:ON " : "BT:OFF ");
    u8g2.print(displayEngine ? "ENG:RUN" : "ENG:OFF");

    u8g2.setCursor(0, 26);
    u8g2.print("SPD:");
    u8g2.print(displaySpeed, 1);
    u8g2.print(" km/h");

    u8g2.setCursor(0, 42);
    u8g2.print("RPM:");
    u8g2.print((int)displayRpm);

    u8g2.setCursor(0, 58);
    u8g2.print("FUEL:");
    u8g2.print(displayFuel, 1);
    u8g2.print("L");

    drawProgressBar(75, 50, 50, 5, (tankCapacity > 0) ? (displayFuel / tankCapacity) : 0);

    drawBtIcon(112, 0, displayBtConnected);
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

    // Приветственный экран (неблокирующий)
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(0, 10); u8g2.print("Car BKV2");
    u8g2.setCursor(0, 25); u8g2.print("ESP32 + OLED");
    u8g2.setCursor(0, 40); u8g2.print("Pub/Sub Arch");
    u8g2.sendBuffer();
    lastHeartbeat = millis();  // Обновляем heartbeat ПЕРЕД задержкой!
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Подписки
    SubscriberOpts pktOpts = {QUEUE_OVERWRITE, 1, false};
    DataBus& db = DataBus::getInstance();
    QueueHandle_t engineQ = db.subscribe(TOPIC_ENGINE_PACK, pktOpts);
    QueueHandle_t tripQ = db.subscribe(TOPIC_TRIP_PACK, pktOpts);
    QueueHandle_t btQ = db.subscribe(TOPIC_TRANSPORT_STATUS, pktOpts);

    // Начальный BT статус из кэша
    BusMessage cached;
    if (db.getCached(TOPIC_TRANSPORT_STATUS, cached) && cached.type == TYPE_BOOL) {
        displayBtConnected = cached.value.b;
    }

    Serial.println("[OLED] Ready (Queue-based)");

    unsigned long lastUpdate = 0;

    while (1) {
        lastHeartbeat = millis();

        // Чтение EnginePack
        BusMessage msg;
        if (engineQ && xQueueReceive(engineQ, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
            EnginePack p; memcpy(&p, msg.value.s, sizeof(EnginePack));
            displaySpeed = p.speed;
            displayRpm = p.rpm;
            displayVoltage = p.voltage;
            displayEngine = p.engine_running;
        }

        // Чтение TripPack
        if (tripQ && xQueueReceive(tripQ, &msg, 0) == pdTRUE && msg.type == TYPE_STRING) {
            TripPack p; memcpy(&p, msg.value.s, sizeof(TripPack));
            displayFuel = p.fuel_level;
            displayConsumption = p.avg_consumption;
        }

        // Чтение BT статуса
        if (btQ && xQueueReceive(btQ, &msg, 0) == pdTRUE && msg.type == TYPE_BOOL) {
            displayBtConnected = msg.value.b;
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
        xTaskCreatePinnedToCore(oledTask, "OLED", 8192, NULL, 2, &oledTaskHandle, 0);
        Serial.println("[OLED] Started (8K stack, P2)");
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
