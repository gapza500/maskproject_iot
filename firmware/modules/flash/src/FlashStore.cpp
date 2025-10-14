
#include "FlashStore.h"
#include <Arduino.h>

bool FlashStore::begin() { return true; }
bool FlashStore::writeRecord(const uint8_t* data, size_t len) {
  (void)data; (void)len; return true;
}
size_t FlashStore::readLatest(uint8_t* out, size_t maxlen) {
  (void)out; (void)maxlen; return 0;
}
