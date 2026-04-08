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

#endif // BT_TRANSPORT_H
