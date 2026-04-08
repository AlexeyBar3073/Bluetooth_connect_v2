// -----------------------------------------------------------------------------
// climate.h
// Климат / сервисные датчики.
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, ServicePack
// -----------------------------------------------------------------------------

#ifndef CLIMATE_H
#define CLIMATE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void climateTask(void* parameter);
void climateStart();
void climateStop();
bool climateIsRunning();

#endif // CLIMATE_H
