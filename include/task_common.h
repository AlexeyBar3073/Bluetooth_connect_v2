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
//   - CMD_OTA_START → все задачи завершаются одинаково
//   - Heartbeat → для loop()-мониторинга
//
// ВЕРСИЯ: 6.8.20 — единый фреймворк задач
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
// TaskContext — общее состояние задачи (встраивается в каждую задачу)
// =============================================================================

typedef struct {
    const char*     name;           // Имя задачи для логов
    bool*           isRunningFlag;  // Указатель на флаг активности
    unsigned long*  lastHeartbeat;  // Указатель на последний heartbeat
    QueueHandle_t   cmdQ;           // Очередь команд (TOPIC_CMD)
} TaskContext;

// =============================================================================
// Инициализация — создаёт очередь команд и подписывает на TOPIC_CMD
// =============================================================================
// Вызывается ВНУТРИ задачи (в начале её цикла).
// Возращает true при успехе, false при ошибке.
//
// Пример использования в calculatorTask():
//   TaskContext ctx;
//   if (!taskInit(&ctx, "Calculator", &isRunningFlag, &lastHeartbeat)) return;
//

bool taskInit(TaskContext* ctx, const char* name, bool* isRunningFlag, unsigned long* lastHeartbeat);

// =============================================================================
// Обработка команд — читает cmdQ и выполняет общие действия
// =============================================================================
// Вызывается в основном цикле задачи.
// Обрабатывает:
//   - CMD_OTA_START → завершает задачу (unsubscribe + vTaskDelete)
//   - Специфичные команды делегируются через callback
//
// callback: функция для обработки команд, НЕ связанных с OTA.
//   Если callback вернёт true — команда обработана.
//   Если false — команда неизвестна (лог).
//
typedef bool (*TaskCmdCallback)(uint8_t cmd);

void taskProcessCommands(TaskContext* ctx, TaskCmdCallback specificHandler);

// =============================================================================
// Heartbeat — обновляет счётчик для loop()-мониторинга
// =============================================================================
// Вызывается в начале каждого цикла while(1).

static inline void taskHeartbeat(TaskContext* ctx) {
    if (ctx && ctx->lastHeartbeat) {
        *ctx->lastHeartbeat = millis();
    }
}

// =============================================================================
// Завершение задачи — общий путь выхода
// =============================================================================
// Устанавливает isRunningFlag = false и вызывает vTaskDelete(NULL).
// Используется как в taskProcessCommands (CMD_OTA_START), так и в специфичной логике.

void taskShutdown(TaskContext* ctx);

#endif // TASK_COMMON_H
