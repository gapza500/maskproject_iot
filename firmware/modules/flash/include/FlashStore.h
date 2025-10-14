
#pragma once
#include <Arduino.h>

class FlashStore {
 public:
  bool begin();
  bool writeRecord(const uint8_t* data, size_t len);
  size_t readLatest(uint8_t* out, size_t maxlen);
};
