// -----------------------------------------------------------------------------
// kline_task.cpp
// K-Line диагностика (опрос ЭБУ, автоопределение протокола).
//
// Назначение:
// - Подписан на TOPIC_CMD (QueuePolicy::FIFO_DROP, depth=5) → kl_* команды
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
// ВЕРСИЯ: 6.1.0 — K-Line на DataRouter (typed topics, module-owned queues)
// -----------------------------------------------------------------------------

#include "kline_task.h"
#include "data_router.h"
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

// --- Очередь команд (создаётся модулем) ---
static QueueHandle_t cmdQueue = NULL;

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
static void processCommands() {
    uint8_t cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        switch ((Command)cmd) {
            case CMD_KL_GET_DTC:
#if DEBUG_LOG
                Serial.println("[KLine] DTC request received (will update KlinePack)");
#endif
                break;

            case CMD_KL_CLEAR_DTC:
                testDtcCount = 0;
                memset(testDtcCodes, 0, sizeof(testDtcCodes));
#if DEBUG_LOG
                Serial.println("[KLine] DTC cleared");
#endif
                break;

            case CMD_KL_RESET_ADAPT:
#if DEBUG_LOG
                Serial.println("[KLine] TCM adaptation reset requested");
#endif
                break;

            case CMD_KL_PUMP_ATF:
#if DEBUG_LOG
                Serial.println("[KLine] ATF pump requested");
#endif
                break;

            case CMD_KL_DETECT_PROTO:
                currentProtocol = 1;
#if DEBUG_LOG
                Serial.println("[KLine] Protocol detection requested");
#endif
                break;

            case CMD_OTA_START:
                Serial.println("[KLine] CMD_OTA_START — shutting down");
                isRunningFlag = false;
                // Безопасный выход — FreeRTOS удалит TCB.
                // klineStop() вызовется из loop() и очистит taskHandle.
                return;
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
    DataRouter& router = DataRouter::getInstance();

    // Создаём очередь для команд (модуль сам создаёт очередь)
    cmdQueue = xQueueCreate(5, sizeof(uint8_t));
    if (cmdQueue == NULL) {
        Serial.println("[KLine] ERROR: Failed to create command queue!");
        isRunningFlag = false;
        vTaskDelete(NULL);
        return;
    }

    // Подписка на команды (FIFO_DROP — не терять команды)
    router.subscribe(TOPIC_CMD, cmdQueue, QueuePolicy::FIFO_DROP);

#if DEBUG_LOG
    Serial.println("[KLine] Task started (DataRouter, simulation mode)");
#endif

    unsigned long lastPublish = 0;

    while (1) {
        lastHeartbeat = millis();

        // Обработка команд
        processCommands();

        // Публикация KlinePack каждые 1000 мс
        unsigned long now = millis();
        if (now - lastPublish >= 1000) {
            lastPublish = now;
            updateTestData();

            KlinePack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version           = 2;
            pack.coolant_temp      = testCoolant;
            pack.atf_temp          = testAtf;
            pack.tcc_lockup        = false;
            pack.selector_position = 3;    // D
            pack.current_gear      = 2;    // 2-я передача
            pack.voltage           = 14.0f + (random(0, 50) / 100.0f);
            pack.fuel_percent      = 55.0f + (random(0, 20) / 10.0f);
            pack.output_shaft_rpm  = 1500.0f + (random(0, 500));
            pack.dtc_count         = (uint8_t)testDtcCount;
            strlcpy(pack.dtc_codes, testDtcCodes, sizeof(pack.dtc_codes));

            router.publishPacket(TOPIC_KLINE_PACK, &pack, sizeof(pack));
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void klineStart() {
    if (!taskHandle) {
        // СРАЗУ — чтобы loop() не думал что crashed
        lastHeartbeat = millis();
        isRunningFlag = true;
        // Ядро 1 — K-Line (диагностика ЭБУ)
        xTaskCreatePinnedToCore(klineTask, "KLine", TASK_STACK_SIZE, NULL, TASK_PRIORITY_KLINE, &taskHandle, 1);
#if DEBUG_LOG
        Serial.println("[KLine] Started (P1)");
#endif
    }
}

void klineStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
    }
    isRunningFlag = false;
    // НЕ удаляем очередь — при рестарте создастся новая, старая освободится кучей.
    // vQueueDelete при асинхронных рестартах вызывает гонки и assert pxQueue.
    cmdQueue = NULL;
#if DEBUG_LOG
    Serial.println("[KLine] Stopped (safe shutdown)");
#endif
}

bool klineIsRunning() { return isRunningFlag && (millis() - lastHeartbeat) < 3000; }
bool klineIsConnected() { return !isRealMode; }

// Legacy-функции для обратной совместимости
void klineRequestDTC() {}
void klineClearDTC() {}
void klineResetTCMAdaptation() {}
void klineStartABSBleed() {}


