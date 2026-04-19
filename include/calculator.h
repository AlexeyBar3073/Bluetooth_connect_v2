// -----------------------------------------------------------------------------
// calculator.h
// Вычислитель (Calculator) — расчётное ядро бортового компьютера.
//
// Назначение:
// - Подписывается на EnginePack (скорость, расстояние, топливо, статус двигателя)
// - Рассчитывает производные параметры: ODO, Trip A/B, Fuel A/B, Avg Consumption
// - Публикует TripPack каждые 1000 мс
// - Обрабатывает команды: reset_trip_a/b, full_tank, correct_odo, reset_avg
// - Реализует формулу: current = base + accumulated
//
// Архитектура:
// Вычислитель НЕ знает откуда приходят данные (Simulator или реальный модуль).
// Он подписан на TOPIC_ENGINE_PACK и TOPIC_TRIP_PACK (для base-значений).
//
// Base-значения (от Storage Task, не меняются до команд):
//   odo_base, trip_a_base, trip_b_base, fuel_trip_a_base, fuel_trip_b_base, fuel_base
//
// Накопленные значения (за поездку, сбрасываются при запуске двигателя):
//   current_distance, current_fuel_used
//
// Формулы расчёта:
//   odo_current     = odo_base + current_distance
//   trip_a_current  = trip_a_base + current_distance
//   trip_b_current  = trip_b_base + current_distance
//   fuel_a_current  = fuel_trip_a_base + current_fuel_used
//   fuel_b_current  = fuel_trip_b_base + current_fuel_used
//   avg_consumption = (current_fuel_used / current_distance) * 100.0
//   fuel_level      = fuel_base - current_fuel_used (если not_fuel = true)
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ Можно:
//   - Добавлять новые формулы расчёта
//   - Добавлять обработку новых команд
//   - Менять частоту публикации TripPack
//   - Добавлять защиту от некорректных данных (проверка диапазонов)
//
// ❌ Нельзя:
//   - Вызывать другие модули напрямую (только через DataBus)
//   - Писать в NVS напрямую (только через Storage Task)
//   - Менять base-значения без команды (reset_*, correct_odo)
//   - Блокировать задачу более 100 мс
//
// Обновление (интеграция с task_common):
// - Автоматическая обработка CMD_OTA_START с полной очисткой ресурсов
// - Единый heartbeat для мониторинга в loop()
// - Регистрация подписок для корректной отписки при завершении
// -----------------------------------------------------------------------------

#ifndef CALCULATOR_H
#define CALCULATOR_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "topics.h"
#include "commands.h"
#include "packets.h"

// =============================================================================
// Публичный API
// =============================================================================

// calculatorTask: FreeRTOS задача вычислителя.
// Подписывается на EnginePack, TripPack, команды.
// Рассчитывает TripPack и публикует в TOPIC_TRIP_PACK каждые 1000 мс.
void calculatorTask(void* parameter);

// calculatorStart: Запускает задачу вычислителя.
void calculatorStart();

// calculatorStop: Останавливает задачу вычислителя.
void calculatorStop();

// calculatorIsRunning: Возвращает true если задача активна.
bool calculatorIsRunning();

#endif // CALCULATOR_H