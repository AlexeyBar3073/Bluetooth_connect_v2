// -----------------------------------------------------------------------------
// protocol_task.h
// Протокол связи с Android (JSON-команды, фракционная телеметрия).
//
// Назначение:
// - Подписка на EnginePack, TripPack, KlinePack, ClimatePack, SettingsPack (очереди)
// - Подписка на TOPIC_MSG_INCOMING → парсинг JSON-команд
// - Фракционная отправка JSON: FAST 100мс, TRIP 500мс, SERVICE 1000мс
// - 13 команд: reset_trip, full_tank, correct_odo, get_cfg, set_cfg, kl_*
// - Координация OTA-обновления (приём ota_update, ota_data, ota_end)
//
// Архитектура: Queue-based, DataRouter, task_common framework
//
// Обновление (интеграция с task_common):
// - Использует taskInit() для унифицированной инициализации
// - Protocol НЕ завершается при CMD_OTA_START (продолжает обработку OTA-чанков)
// - Единый heartbeat для мониторинга в loop()
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#ifndef PROTOCOL_TASK_H
#define PROTOCOL_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =============================================================================
// Публичный API
// =============================================================================

// protocolTask: FreeRTOS задача протокола
void protocolTask(void* parameter);

// protocolStart: Запускает задачу протокола
void protocolStart();

// protocolStop: Останавливает задачу протокола
void protocolStop();

// protocolIsRunning: Возвращает true если задача активна
bool protocolIsRunning();

#endif // PROTOCOL_TASK_H