#include "FlashLogger.h"

FlashLogger::FlashLogger(uint8_t csPin) {
  _cs = csPin;
}

bool FlashLogger::begin(RTC_DS3231 *rtc) {
  _rtc = rtc;
  SPI.begin(23, 21, 22, _cs);
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  delay(10);

  DateTime now = _rtc->now();
  _currentDay = getDayID(now);
  _currentSector = _currentDay % MAX_SECTORS;
  _writeAddr = _currentSector * SECTOR_SIZE;

  for (int i = 0; i < MAX_SECTORS; i++) indexTable[i] = {0, 0, false, false};
  indexTable[_currentSector] = {_currentDay, _currentSector, true, false};

  findLastWritePosition();

  Serial.printf("Logger ready. Day %u, Sector %u, Addr 0x%06lX\n",
                _currentDay, _currentSector, _writeAddr);
  return true;
}

bool FlashLogger::append(const String &json) {
  DateTime now = _rtc->now();
  uint16_t today = getDayID(now);

  if (today != _currentDay) {
    _currentDay = today;
    _currentSector = _currentDay % MAX_SECTORS;
    _writeAddr = _currentSector * SECTOR_SIZE;
    sectorErase(_writeAddr);
    indexTable[_currentSector] = {_currentDay, _currentSector, true, false};
  }

  if ((_writeAddr + PAGE_SIZE) >= ((_currentSector + 1) * SECTOR_SIZE)) {
    moveToNextSector();
  }

  Serial.printf("Writing @0x%06lX  (%02d/%02d/%02d)\n",
                _writeAddr, now.day(), now.month(), now.year() - 2000);

  uint16_t len = json.length() + 1;
  if (len > PAGE_SIZE) len = PAGE_SIZE;

  writeEnable();
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_PP);
  SPI.transfer((_writeAddr >> 16) & 0xFF);
  SPI.transfer((_writeAddr >> 8) & 0xFF);
  SPI.transfer(_writeAddr & 0xFF);
  for (int i = 0; i < len; i++) SPI.transfer(json[i]);
  digitalWrite(_cs, HIGH);
  delay(3);

  _writeAddr += len;
  return true;
}

void FlashLogger::printFormattedLogs() {
  Serial.println("----------------------------------");
  for (int s = 0; s < MAX_SECTORS; s++) {
    if (!indexTable[s].used) continue;

    DateTime now = _rtc->now();
    uint16_t dayID = indexTable[s].dayID;
    uint16_t diff = now.dayOfTheWeek();  // dummy to call now

    // Convert back to approximate date
    DateTime date = now - TimeSpan((getDayID(now) - dayID) * 86400L);
    Serial.printf("date %02d/%02d/%02d\n{\n", date.day(), date.month(), date.year() - 2000);

    uint32_t start = s * SECTOR_SIZE;
    uint8_t buf[PAGE_SIZE];
    for (uint32_t addr = start; addr < (start + SECTOR_SIZE); addr += PAGE_SIZE) {
      readData(addr, buf, PAGE_SIZE);
      for (int i = 0; i < PAGE_SIZE; i++) {
        if (buf[i] != 0xFF && buf[i] != 0x00) Serial.write(buf[i]);
      }
    }
    Serial.println("\n}");
    Serial.println("----------------------------------");
  }
}

void FlashLogger::gc() {
  DateTime now = _rtc->now();
  Serial.println("🧹 Running GC...");

  for (int s = 0; s < MAX_SECTORS; s++) {
    if (!indexTable[s].used || !indexTable[s].pushed) continue;
    if (isOlderThan7Days(now, indexTable[s].dayID)) {
      uint32_t addr = s * SECTOR_SIZE;
      sectorErase(addr);
      indexTable[s].used = false;
      indexTable[s].pushed = false;
      Serial.printf("Erased sector %u (older than 7 days)\n", s);
    }
  }
}

bool FlashLogger::isOlderThan7Days(DateTime now, uint16_t dayID) {
  uint16_t today = getDayID(now);
  if (today > dayID && (today - dayID) >= 7) return true;
  return false;
}

void FlashLogger::moveToNextSector() {
  if (_currentSector + 1 < MAX_SECTORS) {
    _currentSector++;
    _writeAddr = _currentSector * SECTOR_SIZE;
    indexTable[_currentSector] = {_currentDay, _currentSector, true, false};
    sectorErase(_writeAddr);
    Serial.printf("Moved → Sector %u (Addr 0x%06lX)\n", _currentSector, _writeAddr);
  } else {
    Serial.println("⚠️ Flash full! Stop logging.");
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

void FlashLogger::readData(uint32_t addr, uint8_t *buf, uint16_t len) {
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_READ);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  for (int i = 0; i < len; i++) buf[i] = SPI.transfer(0x00);
  digitalWrite(_cs, HIGH);
}

uint16_t FlashLogger::getDayID(DateTime now) {
  // Days since 2000-01-01 (RTClib base)
  return (now.year() - 2000) * 365 + (now.dayOfTheWeek() + now.day());
}

void FlashLogger::findLastWritePosition() {
  uint8_t buf[PAGE_SIZE];
  uint32_t baseAddr = _currentSector * SECTOR_SIZE;
  _writeAddr = baseAddr;

  for (uint32_t addr = baseAddr; addr < (baseAddr + SECTOR_SIZE); addr += PAGE_SIZE) {
    readData(addr, buf, PAGE_SIZE);
    if (buf[0] == 0xFF || buf[0] == 0x00) {
      _writeAddr = addr;
      Serial.printf("Resume write at 0x%06lX (Sector %u)\n", _writeAddr, _currentSector);
      return;
    }
  }
  moveToNextSector();
}
