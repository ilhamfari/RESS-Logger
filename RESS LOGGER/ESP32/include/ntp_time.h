#pragma once

#include <Arduino.h>
#include <time.h>

class NTPTimeManager {
public:
    static void init();
    static void process();
    static bool isTimeSynchronized();
    static uint32_t getEpochSeconds();
    static String getFormattedTime();
    static void forceSync();
    static bool syncToSTM32();
    // Set the system clock directly (HTTP date / browser clock fallbacks).
    static bool setFromEpoch(uint32_t epoch, const char *source);
    static const char *getSource();   // "sntp", "http-date", "browser" or "none"
};
