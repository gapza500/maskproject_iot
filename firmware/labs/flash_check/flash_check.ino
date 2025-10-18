#include <SPI.h>

#define FLASH_CS 4   // Chip Select (choose a free pin)

// W25Q64 commands
#define CMD_RDID  0x9F
#define CMD_READ  0x03
#define CMD_WREN  0x06
#define CMD_PP    0x02
#define CMD_SE    0x20

SPIClass spi(FSPI);

void flashWriteEnable() {
  digitalWrite(FLASH_CS, LOW);
  spi.transfer(CMD_WREN);
  digitalWrite(FLASH_CS, HIGH);
}

void flashReadID() {
  digitalWrite(FLASH_CS, LOW);
  spi.transfer(CMD_RDID);
  byte mfg = spi.transfer(0x00);
  byte memType = spi.transfer(0x00);
  byte capacity = spi.transfer(0x00);
  digitalWrite(FLASH_CS, HIGH);

  Serial.print("Manufacturer ID: 0x");
  Serial.println(mfg, HEX);
  Serial.print("Memory Type: 0x");
  Serial.println(memType, HEX);
  Serial.print("Capacity: 0x");
  Serial.println(capacity, HEX);
}

void flashReadData(uint32_t addr, uint8_t *buf, size_t len) {
  digitalWrite(FLASH_CS, LOW);
  spi.transfer(CMD_READ);
  spi.transfer((addr >> 16) & 0xFF);
  spi.transfer((addr >> 8) & 0xFF);
  spi.transfer(addr & 0xFF);
  for (size_t i = 0; i < len; i++) {
    buf[i] = spi.transfer(0x00);
  }
  digitalWrite(FLASH_CS, HIGH);
}

void flashPageProgram(uint32_t addr, const uint8_t *buf, size_t len) {
  flashWriteEnable();

  digitalWrite(FLASH_CS, LOW);
  spi.transfer(CMD_PP);
  spi.transfer((addr >> 16) & 0xFF);
  spi.transfer((addr >> 8) & 0xFF);
  spi.transfer(addr & 0xFF);
  for (size_t i = 0; i < len; i++) {
    spi.transfer(buf[i]);
  }
  digitalWrite(FLASH_CS, HIGH);

  delay(5);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  // Initialize SPI on hardware pins (MISO=21, MOSI=22, SCK=23, CS=4)
  spi.begin(23, 21, 22, FLASH_CS);

  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);

  Serial.println("W25Q64 Test");

  flashReadID();

  // Test write/read
  const char testData[] = "Hello W25Q64!";
  flashPageProgram(0x000000, (const uint8_t*)testData, sizeof(testData));

  uint8_t buf[20];
  flashReadData(0x000000, buf, sizeof(testData));

  Serial.print("Read back: ");
  for (int i = 0; i < sizeof(testData); i++) {
    Serial.print((char)buf[i]);
  }
  Serial.println();
}

void loop() {}
