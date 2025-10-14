
#pragma once
#include <Arduino.h>

class OledDriver {
 public:
  bool begin();
  void showVersion(const char* v);
  void printLine(uint8_t row, const String& s);
  void clear();
};
