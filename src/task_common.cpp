// -----------------------------------------------------------------------------
// task_common.cpp
// Реализация общего фреймворка задач с полной очисткой ресурсов при OTA.
//
// ВНИМАНИЕ: Логирование подписок ОТКЛЮЧЕНО для экономии памяти!
// Обработчик CMD_OTA_START может вернуть false, чтобы задача НЕ завершалась.
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include "task_common.h"
#include "debug.h"

// =============================================================================
// taskInit — инициализация общего контекста
// =============================================================================
bool taskInit(TaskContext* ctx, const char* name, bool* isRunningFlag, unsigned long* lastHeartbeat) {
    if (!ctx || !name || !isRunningFlag || !lastHeartbeat) {
        return false;
    }

    ctx->name = name;
    ctx->isRunningFlag = isRunningFlag;
    ctx->lastHeartbeat = lastHeartbeat;
    *isRunningFlag = true;
    
    ctx->subscriptionCount = 0;
    ctx->shutdownInProgress = false;
    for (int i = 0; i < TASK_MAX_SUBSCRIPTIONS; i++) {
        ctx->subscriptions[i].topic = TOPIC_COUNT;
        ctx->subscriptions[i].queue = NULL;
    }

    ctx->cmdQ = xQueueCreate(5, sizeof(uint8_t));
    if (!ctx->cmdQ) {
        DBG_PRINTF("[%s] ERROR: Failed to create cmd queue!", name);
        return false;
    }

    DataRouter& dr = DataRouter::getInstance();
    if (!dr.subscribe(TOPIC_CMD, ctx->cmdQ, QueuePolicy::FIFO_DROP)) {
        DBG_PRINTF("[%s] ERROR: Failed to subscribe to TOPIC_CMD!", name);
        vQueueDelete(ctx->cmdQ);
        ctx->cmdQ = NULL;
        return false;
    }
    
    taskRegisterSubscription(ctx, TOPIC_CMD, ctx->cmdQ);

    DBG_PRINTF("[%s] Started", name);
    return true;
}

// =============================================================================
// taskRegisterSubscription — БЕЗ ЛОГИРОВАНИЯ
// =============================================================================
void taskRegisterSubscription(TaskContext* ctx, Topic topic, QueueHandle_t queue) {
    if (!ctx || !queue) {
        return;
    }

    if (ctx->subscriptionCount >= TASK_MAX_SUBSCRIPTIONS) {
        return;
    }

    for (int i = 0; i < ctx->subscriptionCount; i++) {
        if (ctx->subscriptions[i].queue == queue) {
            return;
        }
    }

    ctx->subscriptions[ctx->subscriptionCount].topic = topic;
    ctx->subscriptions[ctx->subscriptionCount].queue = queue;
    ctx->subscriptionCount++;
    // ЛОГ УДАЛЁН
}

// =============================================================================
// taskProcessCommands — обработка команд из очереди
// =============================================================================
void taskProcessCommands(TaskContext* ctx, TaskCmdCallback specificHandler) {
    if (!ctx || !ctx->cmdQ) {
        return;
    }

    uint8_t cmd;
    
    while (xQueueReceive(ctx->cmdQ, &cmd, 0) == pdTRUE) {
        Command c = (Command)cmd;

        if (c == CMD_OTA_START) {
            // По умолчанию задача завершается
            bool shouldShutdown = true;
            
            // Даём обработчику возможность отменить завершение
            if (specificHandler) {
                shouldShutdown = specificHandler(cmd);
            }
            
            if (shouldShutdown) {
                DBG_PRINTF("[%s] OTA START — shutting down", ctx->name);
                taskShutdown(ctx);
                return;
            } else {
                DBG_PRINTF("[%s] OTA START — staying active", ctx->name);
                // Не завершаем задачу, продолжаем обработку
            }
        } else {
            // Остальные команды передаём обработчику
            if (specificHandler) {
                specificHandler(cmd);
            }
        }
    }
}

// =============================================================================
// taskShutdown — полная очистка ресурсов и завершение задачи
// =============================================================================
void taskShutdown(TaskContext* ctx) {
    if (!ctx) return;

    if (ctx->shutdownInProgress) return;
    ctx->shutdownInProgress = true;

    DBG_PRINTF("[%s] Shutting down...", ctx->name);

    DataRouter& dr = DataRouter::getInstance();

    // Отписка от всех топиков
    for (int i = 0; i < ctx->subscriptionCount; i++) {
        Topic topic = ctx->subscriptions[i].topic;
        QueueHandle_t queue = ctx->subscriptions[i].queue;
        
        if (queue != NULL && topic < TOPIC_COUNT) {
            dr.unsubscribe(topic, queue);
        }
        
        ctx->subscriptions[i].topic = TOPIC_COUNT;
        ctx->subscriptions[i].queue = NULL;
    }

    // Удаление очередей
    for (int i = 0; i < ctx->subscriptionCount; i++) {
        QueueHandle_t queue = ctx->subscriptions[i].queue;
        if (queue != NULL) {
            vQueueDelete(queue);
        }
    }

    if (ctx->cmdQ != NULL) {
        vQueueDelete(ctx->cmdQ);
        ctx->cmdQ = NULL;
    }

    ctx->subscriptionCount = 0;

    if (ctx->isRunningFlag) {
        *ctx->isRunningFlag = false;
    }

    DBG_PRINTF("[%s] Shutdown complete", ctx->name);
    vTaskDelete(NULL);
}