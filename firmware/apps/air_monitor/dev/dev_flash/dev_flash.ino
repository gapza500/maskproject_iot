
#include <Arduino.h>
#include "firmware/modules/flash/include/FlashStore.h"

FlashStore store;
void setup(){
  Serial.begin(115200);
  store.begin();
  const char* msg = "hello flash";
  store.writeRecord((const uint8_t*)msg, strlen(msg));
}
void loop(){
  uint8_t buf[32]; size_t n = store.readLatest(buf, sizeof(buf));
  Serial.printf("readLatest: %u bytes\n", (unsigned)n);
  delay(1000);
}
