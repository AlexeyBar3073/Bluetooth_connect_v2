// -----------------------------------------------------------------------------
// bt_transport.h
// Транспортный уровень Bluetooth Classic (SPP).
// -----------------------------------------------------------------------------

#ifndef BT_TRANSPORT_H
#define BT_TRANSPORT_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =============================================================================
// Публичный API
// =============================================================================

void btTransportStart(const char* deviceName);
void btTransportStop();
bool btIsConnected();
bool btIsRunning();      // Проверка, что задача запущена

#endif // BT_TRANSPORT_H