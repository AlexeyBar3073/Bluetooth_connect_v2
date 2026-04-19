// -----------------------------------------------------------------------------
// climate.h
// Климат / сервисные датчики.
//
// Назначение:
// - Публикует ClimatePack в TOPIC_CLIMATE_PACK каждые 1000 мс
// - Режим симуляции: тестовые данные (температуры, датчики)
//
// Архитектура:
// - Подписан на TOPIC_CMD через фреймворк task_common
// - Публикует данные в шину DataRouter
// - При CMD_OTA_START корректно очищает ресурсы и завершается
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#ifndef CLIMATE_H
#define CLIMATE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =============================================================================
// Публичный API
// =============================================================================

// climateTask: FreeRTOS задача климата
// Подписывается на команды, публикует ClimatePack каждые 1000 мс
void climateTask(void* parameter);

// climateStart: Запускает задачу климата
void climateStart();

// climateStop: Останавливает задачу климата
void climateStop();

// climateIsRunning: Возвращает true если задача активна
bool climateIsRunning();

#endif // CLIMATE_H