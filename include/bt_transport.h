// -----------------------------------------------------------------------------
// bt_transport.h
// Транспортный уровень Bluetooth Classic (SPP).
// -----------------------------------------------------------------------------

#ifndef BT_TRANSPORT_H
#define BT_TRANSPORT_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void btTransportStart(const char* deviceName);
void btTransportStop();
bool btIsConnected();
bool btSend(const char* data);  // Отправить строку напрямую через SPP

// OTA status (реализация в ota_handler.cpp)
bool otaIsActive();

#endif // BT_TRANSPORT_H
