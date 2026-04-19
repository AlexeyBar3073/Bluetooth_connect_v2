// -----------------------------------------------------------------------------
// simulator_task.h
// Симулятор автомобиля — виртуальный двигатель.
//
// Назначение:
// - Эмулирует работу двигателя: кнопка запуска (GPIO26), педаль газа (GPIO33)
// - Кнопка габаритов (GPIO27): любое нажатие ≥50мс = переключение габаритов
// - Публикует EnginePack каждые 100 мс в TOPIC_ENGINE_PACK
// - Подписан на TOPIC_CMD (QUEUE_FIFO_DROP, depth=5) для команды full_tank
// - Подписан на TOPIC_SETTINGS_PACK (QUEUE_OVERWRITE, depth=1, retain=true)
//
// Физика:
// - Инерция скорости: разгон 0-100 км/ч за 10 сек (ΔV = 0.2 км/ч за 20мс)
// - RPM: кусочно-линейная кривая (750 idle → 6500 max)
// - Расход: 5 + (speed/100)*10 л/100км
// - Напряжение: 12.7В (выкл) / 13.5-14.5В (вкл)
//
// Обновление (интеграция с task_common):
// - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
// - Единый heartbeat для мониторинга в loop()
// - Регистрация подписок для корректной отписки при завершении
// -----------------------------------------------------------------------------

#ifndef SIMULATOR_TASK_H
#define SIMULATOR_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =============================================================================
// Публичный API
// =============================================================================

// simulatorTask: FreeRTOS задача симулятора
void simulatorTask(void* parameter);

// simulatorStart: Запускает задачу симулятора
void simulatorStart();

// simulatorStop: Останавливает задачу симулятора
void simulatorStop();

// simulatorIsRunning: Возвращает true если задача активна
bool simulatorIsRunning();

// Вспомогательные функции для управления симулятором (используются в loop/отладке)
void simulatorSetSpeed(float speed);
void simulatorSetFuel(float fuel);
void simulatorToggleEngine();
bool simulatorIsEngineRunning();

#endif // SIMULATOR_TASK_H