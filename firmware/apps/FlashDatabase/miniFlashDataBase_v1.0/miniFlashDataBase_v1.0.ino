#include "FlashLogger.h"

FlashLogger logger(4);  // CS pin = 5

void setup() {
  Serial.begin(115200);
  delay(500);

  logger.begin();

  // Simulate logging
  logger.append("{\"temp\":25.5,\"hum\":60}");
  logger.append("{\"temp\":25.7,\"hum\":61}");
  logger.append("{\"temp\":26.0,\"hum\":62}");

  delay(500);
  logger.listSectors();
  logger.readAll();
}

void loop() {
  // Example of periodic logging
  // logger.append("{\"temp\": random(25,28), \"hum\": random(55,65)}");
  // delay(10000);
}
