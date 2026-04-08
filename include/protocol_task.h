// -----------------------------------------------------------------------------
// protocol_task.h
// Протокол связи с Android (JSON-команды, фракционная телеметрия).
//
// Назначение:
// - Подписка на EnginePack, TripPack, ServicePack, SettingsPack (очереди)
// - Подписка на TOPIC_MSG_INCOMING → парсинг JSON-команд
// - Фракционная отправка JSON: FAST 100мс, TRIP 500мс, SERVICE 1000мс
// - 13 команд: reset_trip, full_tank, correct_odo, get_cfg, set_cfg, kl_*
//
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура
// -----------------------------------------------------------------------------

#ifndef PROTOCOL_TASK_H
#define PROTOCOL_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void protocolTask(void* parameter);
void protocolStart();
void protocolStop();
bool protocolIsRunning();

#endif // PROTOCOL_TASK_H
