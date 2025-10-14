
#pragma once
#include <Arduino.h>
#ifndef LOG_LEVEL
#define LOG_LEVEL 1 // 0=off,1=info,2=debug
#endif
#if LOG_LEVEL >= 1
  #define LOGI(...) do { Serial.printf(__VA_ARGS__); Serial.println(); } while(0)
#else
  #define LOGI(...)
#endif
#if LOG_LEVEL >= 2
  #define LOGD(...) do { Serial.printf(__VA_ARGS__); Serial.println(); } while(0)
#else
  #define LOGD(...)
#endif
