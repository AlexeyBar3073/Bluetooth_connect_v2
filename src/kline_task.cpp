// -----------------------------------------------------------------------------
// kline_task.cpp
// K-Line диагностика (опрос ЭБУ, автоопределение протокола).
//
// Назначение:
// - Подписан на TOPIC_CMD (FIFO_DROP, depth=5) → kl_* команды
// - Публикует KlinePack в TOPIC_KLINE_PACK каждые 1000 мс
// - Режим симуляции: тестовые данные (температуры, DTC)
//
// ВАЖНО: K-Line НЕ формирует JSON! Он публикует KlinePack в свой топик.
// Protocol Task подписан, собирает SERVICE JSON.
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
//   - Публиковать KlinePack чаще 1000 мс
//   - Формировать JSON или публиковать в TOPIC_MSG_OUTGOING
//   - Оперировать msg_id / ack_id
//
// ВЕРСИЯ: 5.1.0 — MAJOR: Публикация KlinePack (не ServicePack)
// -----------------------------------------------------------------------------

#include "kline_task.h"
#include "data_bus.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "app_config.h"

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
        if (msg.type != TYPE_INT && msg.type != TYPE_CMD) {
            busMessageFree(&msg);
            continue;
        }

        Command cmd;
        if (msg.type == TYPE_INT) {
            cmd = (Command)msg.value.i;
        } else {
            cmd = msg.cmd.cmd;
        }
        busMessageFree(&msg);

        switch (cmd) {
            case CMD_KL_GET_DTC:
                // K-Line НЕ отправляет JSON! Обновляем данные, Protocol сам соберёт ответ
                Serial.println("[KLine] DTC request received (will update KlinePack)");
                break;

            case CMD_KL_CLEAR_DTC:
                testDtcCount = 0;
                memset(testDtcCodes, 0, sizeof(testDtcCodes));
                Serial.println("[KLine] DTC cleared");
                break;

            case CMD_KL_RESET_ADAPT:
                Serial.println("[KLine] TCM adaptation reset requested");
                break;

            case CMD_KL_PUMP_ATF:
                Serial.println("[KLine] ATF pump requested");
                break;

            case CMD_KL_DETECT_PROTO:
                currentProtocol = 1;
                Serial.println("[KLine] Protocol detection requested");
                break;

            default:
                break;
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

        // Публикация KlinePack каждые 1000 мс
        unsigned long now = millis();
        if (now - lastPublish >= 1000) {
            lastPublish = now;
            updateTestData();

            KlinePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version      = 1;
            pack.coolant_temp = testCoolant;
            pack.atf_temp     = testAtf;
            pack.dtc_count    = (uint8_t)testDtcCount;
            strlcpy(pack.dtc_codes, testDtcCodes, sizeof(pack.dtc_codes));

            db.publishPacket(TOPIC_KLINE_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void klineStart() {
    if (!taskHandle) {
        xTaskCreatePinnedToCore(klineTask, "KLine", TASK_STACK_SIZE, NULL, 2, &taskHandle, 0);
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
bool klineIsConnected() { return !isRealMode; }

// Legacy-функции для обратной совместимости
void klineRequestDTC() {}
void klineClearDTC() {}
void klineResetTCMAdaptation() {}
void klineStartABSBleed() {}


