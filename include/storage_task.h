// -----------------------------------------------------------------------------
// storage_task.h
// NVS хранение (binary blobs).
//
// Назначение:
// - При старте: загрузка TripPack и SettingsPack из NVS → publish в DataRouter
// - В работе: подписка на TripPack и SettingsPack → сохранение в NVS
// - Бинарное хранение (putBytes/getBytes)
//
// Архитектура v6.1.0: DataRouter, типизированные топики, module-owned queues
//
// Обновление (интеграция с task_common):
// - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
// - Единый heartbeat для мониторинга в loop()
// - Регистрация подписок для корректной отписки при завершении
// -----------------------------------------------------------------------------

#ifndef STORAGE_TASK_H
#define STORAGE_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =============================================================================
// Публичный API
// =============================================================================

// storageTask: FreeRTOS задача хранения данных в NVS
void storageTask(void* parameter);

// storageStart: Запускает задачу хранения
void storageStart();

// storageStop: Останавливает задачу хранения
void storageStop();

// storageIsRunning: Возвращает true если задача активна
bool storageIsRunning();

// storageForceSave: Аварийное сохранение (вызывается при пропадании ACC)
void storageForceSave();

#endif // STORAGE_TASK_H