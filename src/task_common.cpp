// -----------------------------------------------------------------------------
// task_common.cpp
// Реализация общего фреймворка задач.
//
// ВЕРСИЯ: 6.8.20
// -----------------------------------------------------------------------------

#include "task_common.h"
#include "debug.h"

// =============================================================================
// taskInit — инициализация общего контекста
// =============================================================================

bool taskInit(TaskContext* ctx, const char* name, bool* isRunningFlag, unsigned long* lastHeartbeat) {
    if (!ctx || !name || !isRunningFlag || !lastHeartbeat) return false;

    ctx->name = name;
    ctx->isRunningFlag = isRunningFlag;
    ctx->lastHeartbeat = lastHeartbeat;
    *isRunningFlag = true;

    // Очередь команд
    ctx->cmdQ = xQueueCreate(5, sizeof(uint8_t));
    if (!ctx->cmdQ) {
        DBG_PRINTF("[%s] ERROR: Failed to create cmd queue!\n", name);
        return false;
    }

    DataRouter& dr = DataRouter::getInstance();
    dr.subscribe(TOPIC_CMD, ctx->cmdQ, QueuePolicy::FIFO_DROP);

    return true;
}

// =============================================================================
// taskProcessCommands — обработка команд из очереди
// =============================================================================

void taskProcessCommands(TaskContext* ctx, TaskCmdCallback specificHandler) {
    if (!ctx || !ctx->cmdQ) return;

    uint8_t cmd;
    while (xQueueReceive(ctx->cmdQ, &cmd, 0) == pdTRUE) {
        Command c = (Command)cmd;

        // --- CMD_OTA_START: общий обработчик для ВСЕХ задач ---
        if (c == CMD_OTA_START) {
            DBG_PRINTF("[%s] CMD_OTA_START — shutting down\n", ctx->name);
            taskShutdown(ctx);
            // vTaskDelete не возвращается — остальной код не выполнится
            return;
        }

        // --- Остальные команды — специфичный обработчик ---
        if (specificHandler) {
            specificHandler(cmd);
        }
    }
}

// =============================================================================
// taskShutdown — завершение задачи
// =============================================================================

void taskShutdown(TaskContext* ctx) {
    if (ctx && ctx->isRunningFlag) {
        *ctx->isRunningFlag = false;
    }
    vTaskDelete(NULL);
}
