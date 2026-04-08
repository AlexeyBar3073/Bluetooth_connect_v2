// -----------------------------------------------------------------------------
// storage_task.h
// NVS хранение (binary blobs).
// ВЕРСИЯ: 5.0.0 — MAJOR: Queue-архитектура
// -----------------------------------------------------------------------------

#ifndef STORAGE_TASK_H
#define STORAGE_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void storageTask(void* parameter);
void storageStart();
void storageStop();
bool storageIsRunning();
void storageForceSave();

#endif // STORAGE_TASK_H
