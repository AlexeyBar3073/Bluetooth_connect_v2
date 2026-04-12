// -----------------------------------------------------------------------------
// climate.cpp
// Модуль климата и сервисных датчиков.
//
// Назначение:
// - Публикует ClimatePack в TOPIC_CLIMATE_PACK каждые 1000 мс
// - Режим симуляции: тестовые данные
//
// ВАЖНО: Climate НЕ формирует JSON! Он публикует ClimatePack в свой топик.
// Protocol Task подписан, собирает SERVICE JSON.
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ❌ нельзя:
//   - Формировать JSON или публиковать в TOPIC_MSG_OUTGOING
//   - Оперировать msg_id / ack_id
//
// ВЕРСИЯ: 6.1.0 — Climate на DataRouter (typed topics, module-owned queues)
// -----------------------------------------------------------------------------

#include "climate.h"
#include "data_router.h"
#include "topics.h"
#include "packets.h"
#include "app_config.h"

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
// Публикует ClimatePack каждые 1000 мс.
// Данные климата НЕ пересекаются с KlinePack — каждый модуль в своём топике.
//
void climateTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& router = DataRouter::getInstance();

#if DEBUG_LOG
    Serial.println("[Climate] Task started (DataRouter, Simulation mode)");
#endif

    unsigned long lastPublish = 0;

    while (1) {
        lastHeartbeat = millis();
        unsigned long now = millis();

        if (now - lastPublish >= 1000) {
            lastPublish = now;
            updateTestData();

            ClimatePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version = 1;
            pack.interior_temp = testInterior;
            pack.exterior_temp = testExterior;
            pack.tire_pressure = testTire;
            pack.washer_level = testWash;

            router.publishPacket(TOPIC_CLIMATE_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void climateStart() {
    if (!taskHandle) {
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunningFlag = true;
        // Ядро 1 — Climate (температура, сервис)
        xTaskCreatePinnedToCore(climateTask, "Climate", TASK_STACK_CLIMATE, NULL, TASK_PRIORITY_CLIMATE, &taskHandle, 1);
#if DEBUG_LOG
        Serial.println("[Climate] Started (DataRouter)");
#endif
    }
}

void climateStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
#if DEBUG_LOG
        Serial.println("[Climate] Stopped");
#endif
    }
}

bool climateIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 3000; }
