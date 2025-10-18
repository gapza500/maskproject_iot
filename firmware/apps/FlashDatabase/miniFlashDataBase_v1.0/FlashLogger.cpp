#include "FlashLogger.h"

FlashLogger::FlashLogger(uint8_t csPin) {
  _cs = csPin;
}

bool FlashLogger::begin() {
  SPI.begin(23, 21, 22, _cs);
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  delay(10);

  _currentDay = getDayID();
  _currentSector = _currentDay; // each day starts from its own index
  _writeAddr = _currentSector * SECTOR_SIZE;

  // init index
  for (int i = 0; i < MAX_SECTORS; i++) {
    indexTable[i] = {0, 0, false};
  }
  indexTable[_currentSector] = {_currentDay, _currentSector, true};

  findLastWritePosition();

  Serial.printf("Logger ready. Day %u, Sector %u, Next write @ 0x%06lX\n",
                _currentDay, _currentSector, _writeAddr);
  return true;
}

bool FlashLogger::append(const String &json) {
  uint32_t today = getDayID();
  if (today != _currentDay) {
    _currentDay = today;
    _currentSector = _currentDay;
    _writeAddr = _currentSector * SECTOR_SIZE;
    sectorErase(_writeAddr);
    indexTable[_currentSector] = {_currentDay, _currentSector, true};
  }

  // if full, move to next sector
  if ((_writeAddr + PAGE_SIZE) >= ((_currentSector + 1) * SECTOR_SIZE)) {
    Serial.println("Sector full → moving to next sector");
    moveToNextSector();
  }

  Serial.printf("Writing @ 0x%06lX (Sector %u)\n", _writeAddr, _currentSector);
  uint16_t len = json.length() + 1;
  if (len > PAGE_SIZE) len = PAGE_SIZE;

  writeEnable();
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_PP);
  SPI.transfer((_writeAddr >> 16) & 0xFF);
  SPI.transfer((_writeAddr >> 8) & 0xFF);
  SPI.transfer(_writeAddr & 0xFF);
  for (int i = 0; i < len; i++)
    SPI.transfer(json[i]);
  digitalWrite(_cs, HIGH);
  delay(3);

  _writeAddr += len;
  return true;
}

void FlashLogger::moveToNextSector() {
  if (_currentSector + 1 < MAX_SECTORS) {
    _currentSector++;
    _writeAddr = _currentSector * SECTOR_SIZE;
    indexTable[_currentSector] = {_currentDay, _currentSector, true};
    sectorErase(_writeAddr);
    Serial.printf("Switched to Sector %u (Addr 0x%06lX)\n", _currentSector, _writeAddr);
  } else {
    Serial.println("⚠️ Flash full! Logging stopped to prevent data loss.");
  }
}

void FlashLogger::readAll() {
  Serial.println("=== Reading All Logs ===");
  for (int s = 0; s < MAX_SECTORS; s++) {
    if (!indexTable[s].used) continue;
    Serial.printf("\n[Day %u | Sector %u @ 0x%06lX]\n",
                  indexTable[s].dayID, indexTable[s].sectorID,
                  (uint32_t)s * SECTOR_SIZE);

    uint32_t start = s * SECTOR_SIZE;
    uint8_t buf[PAGE_SIZE];

    for (uint32_t addr = start; addr < (start + SECTOR_SIZE); addr += PAGE_SIZE) {
      readData(addr, buf, PAGE_SIZE);
      for (int i = 0; i < PAGE_SIZE; i++) {
        if (buf[i] != 0xFF && buf[i] != 0x00)
          Serial.write(buf[i]);
      }
    }
  }
  Serial.println();
}

void FlashLogger::listSectors() {
  Serial.println("=== Active Sector List ===");
  for (int i = 0; i < MAX_SECTORS; i++) {
    if (indexTable[i].used)
      Serial.printf("Day %u → Sector %u (Addr 0x%06lX)\n",
                    indexTable[i].dayID, indexTable[i].sectorID,
                    (uint32_t)i * SECTOR_SIZE);
  }
}

void FlashLogger::writeEnable() {
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_WREN);
  digitalWrite(_cs, HIGH);
}

void FlashLogger::sectorErase(uint32_t addr) {
  writeEnable();
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_SE);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  digitalWrite(_cs, HIGH);
  delay(60);
}

void FlashLogger::pageProgram(uint32_t addr, const uint8_t *buf, uint16_t len) {
  writeEnable();
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_PP);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  for (uint16_t i = 0; i < len; i++)
    SPI.transfer(buf[i]);
  digitalWrite(_cs, HIGH);
  delay(5);
}

void FlashLogger::readData(uint32_t addr, uint8_t *buf, uint16_t len) {
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_READ);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  for (int i = 0; i < len; i++)
    buf[i] = SPI.transfer(0x00);
  digitalWrite(_cs, HIGH);
}

uint32_t FlashLogger::getDayID() {
  time_t now = millis() / 1000;
  return now / (24 * 60 * 60);
}

void FlashLogger::findLastWritePosition() {
  uint8_t buf[PAGE_SIZE];
  uint32_t baseAddr = _currentSector * SECTOR_SIZE;
  _writeAddr = baseAddr;

  for (uint32_t addr = baseAddr; addr < (baseAddr + SECTOR_SIZE); addr += PAGE_SIZE) {
    readData(addr, buf, PAGE_SIZE);
    if (buf[0] == 0xFF || buf[0] == 0x00) {
      _writeAddr = addr;
      Serial.printf("Resumed write at 0x%06lX (Sector %u)\n", _writeAddr, _currentSector);
      return;
    }
  }

  Serial.println("Sector appears full, moving to next.");
  moveToNextSector();
}
