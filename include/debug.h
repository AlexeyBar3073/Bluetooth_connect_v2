// -----------------------------------------------------------------------------
// debug.h
// Макросы для централизованного управления отладочным выводом.
// В Release-сборке (DEBUG_LOG=0) не компилируется НИЧЕГО.
// В Debug-сборке (DEBUG_LOG=1) используется стековый буфер, без фрагментации кучи.
// -----------------------------------------------------------------------------

#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>

// =============================================================================
// Отладочные макросы
// =============================================================================

#if DEBUG_LOG
    // Debug-сборка: логи включены
    #define DBG_PRINT(msg)      Serial.print(msg)
    #define DBG_PRINTLN(msg)    Serial.println(msg)
    #define DBG_NEWLINE()       Serial.println()
    
    // Форматированный вывод с буфером на СТЕКЕ (не фрагментирует кучу)
    #define DBG_PRINTF(fmt, ...) do { \
        char _dbg_buf[64]; \
        snprintf(_dbg_buf, sizeof(_dbg_buf), fmt, ##__VA_ARGS__); \
        Serial.print(_dbg_buf); \
        Serial.println(""); \
    } while(0)
    
#else
    // Release-сборка: ВСЕ логи исчезают, не занимают ни байта
    #define DBG_PRINT(msg)      do {} while(0)
    #define DBG_PRINTLN(msg)    do {} while(0)
    #define DBG_NEWLINE()       do {} while(0)
    #define DBG_PRINTF(fmt, ...) do {} while(0)
    
#endif // DEBUG_LOG

#endif // DEBUG_H