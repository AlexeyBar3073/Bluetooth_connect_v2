// -----------------------------------------------------------------------------
// simulator_task.h
// Симулятор автомобиля — виртуальный двигатель.
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура, EnginePack
// -----------------------------------------------------------------------------

#ifndef SIMULATOR_TASK_H
#define SIMULATOR_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void simulatorTask(void* parameter);
void simulatorStart();
void simulatorStop();
bool simulatorIsRunning();

void simulatorSetSpeed(float speed);
void simulatorSetFuel(float fuel);
void simulatorToggleEngine();
bool simulatorIsEngineRunning();

#endif // SIMULATOR_TASK_H
