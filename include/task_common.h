// -----------------------------------------------------------------------------
// task_common.h
// Общий фреймворк задач FreeRTOS — единая архитектура для всех модулей.
//
// Все задачи следуют одному паттерну:
//   1. Инициализация → создание очередей
//   2. Подписка → регистрация в DataRouter
//   3. Основной цикл → команды + специфичная логика
//
// Общие обработчики:
//   - CMD_OTA_START → все задачи завершаются с полной очисткой ресурсов
//   - Heartbeat → для loop()-мониторинга
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#ifndef TASK_COMMON_H
#define TASK_COMMON_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "data_router.h"
#include "topics.h"
#include "commands.h"

// =============================================================================
// Максимальное количество подписок, которые может иметь одна задача
// =============================================================================
#define TASK_MAX_SUBSCRIPTIONS 8

// =============================================================================
// Структура для хранения информации об одной подписке задачи
// =============================================================================
typedef struct {
    Topic topic;                // Топик, на который выполнена подписка
    QueueHandle_t queue;        // Очередь, связанная с этой подпиской
} TaskSubscription;

// =============================================================================
// TaskContext — общее состояние задачи (встраивается в каждую задачу)
// =============================================================================
typedef struct {
    const char*     name;               // Имя задачи для логов
    bool*           isRunningFlag;      // Указатель на флаг активности
    unsigned long*  lastHeartbeat;      // Указатель на последний heartbeat
    QueueHandle_t   cmdQ;               // Очередь команд (TOPIC_CMD)
    
    // === Управление подписками для корректной очистки при OTA ===
    TaskSubscription subscriptions[TASK_MAX_SUBSCRIPTIONS];  // Массив подписок
    uint8_t         subscriptionCount;                       // Количество активных подписок
    bool            shutdownInProgress;                      // Флаг для предотвращения рекурсии
} TaskContext;

// =============================================================================
// Публичные функции
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Инициализация — создаёт очередь команд и подписывает на TOPIC_CMD
bool taskInit(TaskContext* ctx, const char* name, bool* isRunningFlag, unsigned long* lastHeartbeat);

// Регистрация подписки — сохраняет информацию для последующей очистки
void taskRegisterSubscription(TaskContext* ctx, Topic topic, QueueHandle_t queue);

// Обработка команд — читает cmdQ и выполняет общие действия
typedef bool (*TaskCmdCallback)(uint8_t cmd);
void taskProcessCommands(TaskContext* ctx, TaskCmdCallback specificHandler);

// Завершение задачи — полная очистка ресурсов перед удалением
void taskShutdown(TaskContext* ctx);

#ifdef __cplusplus
}
#endif

// Heartbeat — обновляет счётчик для loop()-мониторинга
static inline void taskHeartbeat(TaskContext* ctx) {
    if (ctx && ctx->lastHeartbeat) {
        *ctx->lastHeartbeat = millis();
    }
}

#endif // TASK_COMMON_H