// -----------------------------------------------------------------------------
// kline_task.cpp
// K-Line диагностика (опрос ЭБУ, автоопределение протокола).
//
// Назначение:
// - Подписан на TOPIC_CMD (FIFO_DROP, depth=5) → kl_* команды
// - Публикует ServicePack в TOPIC_SERVICE_PACK каждые 1000 мс
// - Режим симуляции: тестовые данные (температуры, DTC)
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ можно:
//   - Добавлять реальный K-Line опрос ЭБУ
//   - Добавлять новые PID
//   - Менять логику автоопределения
//
// ❌ нельзя:
//   - Блокировать >500 мс
//   - Публиковать ServicePack чаще 1000 мс
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, ServicePack
// -----------------------------------------------------------------------------

#include "kline_task.h"
#include "data_bus.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"

// =============================================================================
// Глобальные переменные
// =============================================================================

static TaskHandle_t  taskHandle     = NULL;
static bool          isRunningFlag  = false;
static unsigned long lastHeartbeat  = 0;
static int           currentProtocol = 0;
static bool          isRealMode     = false;

// --- Тестовые данные ---
static float    testCoolant   = 90.0f;
static float    testAtf       = 75.0f;
static int      testDtcCount  = 2;
static char     testDtcCodes[64] = "P0135;P0141";
static unsigned long lastTestUpdate = 0;

// =============================================================================
// updateTestData
// =============================================================================
static void updateTestData() {
    unsigned long now = millis();
    if (now - lastTestUpdate < 1000) return;
    lastTestUpdate = now;
    testCoolant = 85.0f + (random(0, 20) / 10.0f);
    testAtf     = 70.0f + (random(0, 15) / 10.0f);
}

// =============================================================================
// processCommands: Обработка очереди команд TOPIC_CMD
// =============================================================================
static void processCommands(QueueHandle_t q, DataBus& db) {
    BusMessage msg;
    while (xQueueReceive(q, &msg, 0) == pdTRUE) {
        if (msg.type != TYPE_CMD) continue;

        switch (msg.cmd.cmd) {
            case CMD_KL_GET_DTC: {
                char resp[128];
                snprintf(resp, sizeof(resp), "{\"ack_id\":%d,\"dtc\":\"%s\",\"count\":%d}\n",
                         msg.cmd.msg_id, testDtcCodes, testDtcCount);
                db.publish(TOPIC_MSG_OUTGOING, resp);
                break;
            }
            case CMD_KL_CLEAR_DTC: {
                testDtcCount = 0;
                strcpy(testDtcCodes, "");
                char ack[32];
                snprintf(ack, sizeof(ack), "{\"ack_id\":%d}\n", msg.cmd.msg_id);
                db.publish(TOPIC_MSG_OUTGOING, ack);
                break;
            }
            case CMD_KL_RESET_ADAPT:
            case CMD_KL_PUMP_ATF: {
                char ack[32];
                snprintf(ack, sizeof(ack), "{\"ack_id\":%d}\n", msg.cmd.msg_id);
                db.publish(TOPIC_MSG_OUTGOING, ack);
                break;
            }
            case CMD_KL_DETECT_PROTO: {
                // Симуляция автоопределения
                currentProtocol = 1;
                char ack[32];
                snprintf(ack, sizeof(ack), "{\"ack_id\":%d}\n", msg.cmd.msg_id);
                db.publish(TOPIC_MSG_OUTGOING, ack);
                break;
            }
            default: break;
        }
    }
}

// =============================================================================
// klineTask — Главная задача
// =============================================================================

void klineTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataBus& db = DataBus::getInstance();

    // Подписка на команды (FIFO_DROP, depth=5)
    SubscriberOpts cmdOpts = {QUEUE_FIFO_DROP, 5, false};
    QueueHandle_t cmdQ = db.subscribe(TOPIC_CMD, cmdOpts);

    Serial.println("[KLine] Task started (Queue-based, simulation mode)");

    unsigned long lastPublish = 0;

    while (1) {
        lastHeartbeat = millis();

        // Обработка команд
        if (cmdQ) processCommands(cmdQ, db);

        // Публикация ServicePack каждые 1000 мс
        unsigned long now = millis();
        if (now - lastPublish >= 1000) {
            lastPublish = now;
            updateTestData();

            ServicePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version      = 1;
            pack.coolant_temp = testCoolant;
            pack.atf_temp     = testAtf;
            pack.dtc_count    = (uint8_t)testDtcCount;
            strlcpy(pack.dtc_codes, testDtcCodes, sizeof(pack.dtc_codes));
            // interior/exterior temp, tire, wash — от Climate Task

            db.publishPacket(TOPIC_SERVICE_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void klineStart() {
    if (!taskHandle) {
        xTaskCreatePinnedToCore(klineTask, "KLine", 8192, NULL, 2, &taskHandle, 0);
        Serial.println("[KLine] Started (8K stack, P2)");
    }
}

void klineStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        Serial.println("[KLine] Stopped");
    }
}

bool klineIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 3000; }

// Legacy-функции для совместимости с Protocol Task
void klineRequestDTC() {}
void klineClearDTC() {}
void klineResetTCMAdaptation() {}
void klineStartABSBleed() {}
