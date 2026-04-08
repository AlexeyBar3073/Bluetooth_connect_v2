// -----------------------------------------------------------------------------
// climate.cpp
// Модуль климата и сервисных датчиков.
//
// Назначение:
// - Публикует ServicePack в TOPIC_SERVICE_PACK каждые 1000 мс
// - Режим симуляции: тестовые данные
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, ServicePack
// -----------------------------------------------------------------------------

#include "climate.h"
#include "data_bus.h"
#include "topics.h"
#include "packets.h"

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;

static float  testInterior = 22.0f, testExterior = 15.0f;
static bool   testTire = false, testWash = false;
static unsigned long lastUpdate = 0;

static void updateTestData() {
    unsigned long now = millis();
    if (now - lastUpdate < 5000) return;
    lastUpdate = now;
    testInterior = 20.0f + (random(0, 50) / 10.0f);
    testExterior = 10.0f + (random(0, 80) / 10.0f);
    testTire = (random(0, 100) < 5);
    testWash = (random(0, 100) < 3);
}

// =============================================================================
// climateTask — Главная задача
// =============================================================================
//
// Публикует ServicePack каждые 1000 мс.
// Данные климата дополняют данные от K-Line (temperatures, DTC).
//
void climateTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataBus& db = DataBus::getInstance();

    Serial.println("[Climate] Task started (Simulation mode)");

    unsigned long lastPublish = 0;

    while (1) {
        lastHeartbeat = millis();
        unsigned long now = millis();

        if (now - lastPublish >= 1000) {
            lastPublish = now;
            updateTestData();

            // Публикуем ТОЛЬКО климат-поля, остальные (coolant, atf, dtc) — от K-Line
            // Но ServicePack — единый пакет. K-Line публикует свой,
            // Protocol Task объединяет в кэше.
            ServicePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version = 1;
            pack.interior_temp = testInterior;
            pack.exterior_temp = testExterior;
            pack.tire_pressure = testTire;
            pack.washer_level = testWash;
            // coolant_temp, atf_temp, dtc — нули (K-Line заполнит свой ServicePack)

            db.publishPacket(TOPIC_SERVICE_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void climateStart() {
    if (!taskHandle) {
        xTaskCreatePinnedToCore(climateTask, "Climate", 2048, NULL, 1, &taskHandle, 0);
        Serial.println("[Climate] Started");
    }
}

void climateStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        Serial.println("[Climate] Stopped");
    }
}

bool climateIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 3000; }
