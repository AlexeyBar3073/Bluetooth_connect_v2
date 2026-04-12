// -----------------------------------------------------------------------------
// ota_handler.h
// OTA обновление прошивки через JSON-протокол.
// -----------------------------------------------------------------------------

#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>
#include <stdbool.h>
#include <stddef.h>

// lifecycle
bool otaBegin(size_t firmwareSize);
bool otaWriteChunk(const char* base64Data);
bool otaFinalize();

// status
int  otaGetProgress();
// otaIsActive() declared in bt_transport.h

#endif // OTA_HANDLER_H
