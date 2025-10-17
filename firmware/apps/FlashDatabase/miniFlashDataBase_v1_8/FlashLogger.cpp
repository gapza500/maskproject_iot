#include "FlashLogger.h"
#include <string.h>

// ===== CRC16 (Modbus/A001) =====
uint16_t FlashLogger::crc16(const uint8_t* data, uint32_t len, uint16_t seed) {
  uint16_t crc = seed;
  for (uint32_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

// ===== ctor =====
FlashLogger::FlashLogger(uint8_t csPin) : _cs(csPin) {}

// ===== begin =====
bool FlashLogger::begin(RTC_DS3231* rtc) {
  _rtc = rtc;

  SPI.begin(23, 21, 22, _cs);
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  delay(5);

  for (int i = 0; i < MAX_SECTORS; ++i) _index[i] = {false, 0, false, 0};

  if (!loadFactoryInfo()) {
    memset(&_factory, 0, sizeof(_factory));
    _factory.magic = 0x46414354UL; // 'FACT'
    strncpy(_factory.model, "AirMonitor C6", sizeof(_factory.model)-1);
    strncpy(_factory.flashModel, "W25Q64JV", sizeof(_factory.flashModel)-1);
    strncpy(_factory.deviceId, "000000000000", sizeof(_factory.deviceId)-1);
    _factory.firstDayID = dayIDFromDateTime(_rtc->now());
    _factory.defaultDateStyle = (uint8_t)_dateStyle;
    _factory.totalEraseOps = 0;
    _factory.bootCounter = 0;
    _factory.startHint = 0;
    _factory.badCount = 0;
    sectorErase(sectorBaseAddr(FACTORY_SECTOR), false);
    saveFactoryInfo();
  } else {
    if (_factory.defaultDateStyle >= 1 && _factory.defaultDateStyle <= 3)
      _dateStyle = (DateStyle)_factory.defaultDateStyle;
  }

  // bump boot counter & set generation
  _factory.bootCounter++;
  saveFactoryInfo();
  _generation = _factory.bootCounter;

  scanAllSectorsBuildIndex();

  DateTime now = _rtc->now();
  _currentDay = dayIDFromDateTime(now);

  selectOrCreateTodaySector();
  if (_currentSector >= 0) {
    findLastWritePositionInSector(_currentSector);
    _writeAddr = _index[_currentSector].writePtr;
  } else {
    Serial.println("FlashLogger: no sector available!");
    return false;
  }

  _seqCounter  = 0;
  _todayBytes  = 0;
  _lowSpace    = false;

  Serial.printf("FlashLogger v1.8 ready. Gen=%lu Day=%u Sector=%d Next=0x%06lX\n",
                (unsigned long)_generation, _currentDay, _currentSector, _writeAddr);
  return true;
}

// ===== append (atomic: header -> payload -> commit) =====
bool FlashLogger::append(const String& json) {
  if (_currentSector < 0) return false;

  // Newline-terminated payload (no NUL)
  String payload = json;
  if (payload.isEmpty() || payload[payload.length()-1] != '\n') payload += '\n';
  const uint16_t payLen = (uint16_t)payload.length();
  const uint32_t need   = sizeof(RecordHeader) + payLen + 1; // + commit

  // New day?
  uint16_t today = dayIDFromDateTime(_rtc->now());
  if (today != _currentDay) {
    _currentDay  = today;
    _todayBytes  = 0;
    _lowSpace    = false;

    if (!moveToNextSectorSameDay()) {
      // find any empty sector (round-robin is in selectOrCreate...)
      selectOrCreateTodaySector();
      if (_currentSector < 0) {
        Serial.println("FlashLogger: no sector to start new day.");
        return false;
      }
    }
  }

  // Back-pressure (optional)
  if (_maxDailyBytes) {
    if (_todayBytes + need > _maxDailyBytes) {
      _lowSpace = true;
      Serial.println("Back-pressure: max daily bytes exceeded, append blocked.");
      return false;
    }
  }

  // Sector full?
  if (!sectorHasSpace(_currentSector, need)) {
    if (!moveToNextSectorSameDay()) {
      Serial.println("FlashLogger: out of sectors; append aborted.");
      return false;
    }
  }

  // Build header
  RecordHeader rh{};
  rh.len   = payLen;
  rh.ts    = _rtc->now().secondstime();
  rh.seq   = _seqCounter++;
  rh.flags = 0;
  rh.rsv   = 0;
  rh.crc   = crc16((const uint8_t*)payload.c_str(), payLen, 0xFFFF);

  // Write: header
  pageProgram(_index[_currentSector].writePtr, (const uint8_t*)&rh, sizeof(rh));
  if (!verifyWrite(_index[_currentSector].writePtr, (const uint8_t*)&rh, sizeof(rh))) {
    Serial.println("Header verify FAIL (append), aborting record.");
    return false;
  }
  _index[_currentSector].writePtr += sizeof(rh);

  // Write: payload
  pageProgram(_index[_currentSector].writePtr, (const uint8_t*)payload.c_str(), payLen);
  if (!verifyWrite(_index[_currentSector].writePtr, (const uint8_t*)payload.c_str(), payLen)) {
    Serial.println("Payload verify FAIL (append), aborting record.");
    return false;
  }
  _index[_currentSector].writePtr += payLen;

  // Write: commit
  uint8_t cm = REC_COMMIT;
  pageProgram(_index[_currentSector].writePtr, &cm, 1);
  if (!verifyWrite(_index[_currentSector].writePtr, &cm, 1)) {
    Serial.println("Commit verify FAIL (append), aborting record.");
    return false;
  }
  _index[_currentSector].writePtr += 1;

  _writeAddr  = _index[_currentSector].writePtr;
  _todayBytes += need;

  Serial.printf("APPEND @0x%06lX (sec %d) len=%u\n",
                _writeAddr - need, _currentSector, (unsigned)payLen);
  return true;
}

// ===== formatted print (CRC + commit enforced) =====
void FlashLogger::printFormattedLogs() {
  Serial.println("----------------------------------");
  int lastPrintedDay = -1;

  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;

    uint16_t day = _index[s].dayID;
    if ((int)day != lastPrintedDay) {
      DateTime now = _rtc->now();
      int32_t delta = (int32_t)dayIDFromDateTime(now) - (int32_t)day;
      DateTime dt = now - TimeSpan(delta * 86400L);

      char dateBuf[20];
      formatDate(dt, dateBuf, sizeof(dateBuf));
      Serial.printf("date %s\n{\n", dateBuf);
      lastPrintedDay = (int)day;
    }

    printSectorData(s);

    bool nextSameDay = false;
    for (int t = s + 1; t < MAX_SECTORS; ++t) {
      if (_index[t].present && _index[t].dayID == (uint16_t)lastPrintedDay) {
        nextSameDay = true; break;
      }
    }
    if (!nextSameDay) {
      Serial.println("}\n----------------------------------");
    }
    yield();
  }
}

// ===== raw dump (valid records only) =====
void FlashLogger::readAll() {
  Serial.println("=== RAW DUMP (valid records only) ===");
  uint8_t b;

  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;

    uint32_t base = sectorBaseAddr(s);
    Serial.printf("\n[SECTOR %d @ 0x%06lX] dayID=%u pushed=%d\n",
                  s, base, _index[s].dayID, (int)_index[s].pushed);

    uint32_t ptr = base + sizeof(SectorHeader);
    while (ptr + sizeof(RecordHeader) < base + SECTOR_SIZE) {
      RecordHeader rh;
      readData(ptr, (uint8_t*)&rh, sizeof(rh));
      if (rh.len == 0xFFFF || rh.len == 0x0000) break;
      if (ptr + sizeof(rh) + rh.len + 1 > base + SECTOR_SIZE) break;

      // read payload + compute crc
      uint32_t p = ptr + sizeof(rh);
      uint16_t remaining = rh.len;
      uint16_t crc = 0xFFFF;
      static uint8_t buf[PAGE_SIZE];

      while (remaining) {
        uint16_t chunk = min<uint16_t>(remaining, PAGE_SIZE);
        readData(p, buf, chunk);
        crc = crc16(buf, chunk, crc);
        for (int i=0;i<chunk;i++) Serial.write(buf[i]);
        p += chunk;
        remaining -= chunk;
        yield();
      }

      // commit check
      readData(ptr + sizeof(rh) + rh.len, &b, 1);
      if (b != REC_COMMIT || crc != rh.crc) {
        Serial.println(F("[corrupt/partial record skipped]"));
        break;
      }

      ptr += sizeof(rh) + rh.len + 1;
    }
  }
  Serial.println("\n=== END RAW DUMP ===");
}

// ===== push marking =====
void FlashLogger::markDayPushed(uint16_t dayID) {
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (_index[s].present && _index[s].dayID == dayID) {
      _index[s].pushed = true;
      SectorHeader hdr;
      if (readSectorHeader(s, hdr)) {
        hdr.pushed = 1;
        pageProgram(sectorBaseAddr(s), (const uint8_t*)&hdr, sizeof(SectorHeader));
        verifyWrite(sectorBaseAddr(s), (const uint8_t*)&hdr, sizeof(SectorHeader));
      }
    }
  }
  Serial.printf("Marked day %u as pushed.\n", dayID);
}
void FlashLogger::markCurrentDayPushed() { markDayPushed(_currentDay); }

// ===== gc (generation-aware note in print) =====
void FlashLogger::gc() {
  DateTime now = _rtc->now();
  uint16_t todayID = dayIDFromDateTime(now);
  Serial.println("🧹 GC: checking sectors...");

  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    if (!_index[s].pushed) continue;

    SectorHeader hdr;
    if (!readSectorHeader(s, hdr)) continue;

    if (isOlderThanNDays(todayID, hdr.dayID, 7)) {
      uint32_t base = sectorBaseAddr(s);
      sectorErase(base); // counts erase & verifies (quarantine if fail)
      _index[s] = {false, 0, false, 0};
      Serial.printf("  erased sector %d (day=%u, gen=%lu)\n", s, hdr.dayID, (unsigned long)hdr.generation);
    }
    yield();
  }
}

// ===== setDateStyle =====
void FlashLogger::setDateStyle(uint8_t style) {
  if (style < 1 || style > 3) return;
  _dateStyle = (DateStyle)style;
  _factory.defaultDateStyle = style;
  saveFactoryInfo();
}

// ===== low-level =====
void FlashLogger::writeEnable() {
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_WREN);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void FlashLogger::readData(uint32_t addr, uint8_t* buf, uint16_t len) {
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_READ);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer(addr         & 0xFF);
  for (uint16_t i = 0; i < len; ++i) buf[i] = SPI.transfer(0x00);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

// chunk-safe page program (won't cross 256-byte page boundary)
void FlashLogger::pageProgram(uint32_t addr, const uint8_t* buf, uint32_t len) {
  while (len) {
    uint32_t pageOff = addr & (PAGE_SIZE - 1);
    uint32_t room    = PAGE_SIZE - pageOff;
    uint32_t n       = (len < room) ? len : room;

    writeEnable();
    SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_cs, LOW);
    SPI.transfer(CMD_PP);
    SPI.transfer((addr >> 16) & 0xFF);
    SPI.transfer((addr >> 8)  & 0xFF);
    SPI.transfer(addr         & 0xFF);
    for (uint32_t i = 0; i < n; ++i) SPI.transfer(buf[i]);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
    delay(3);

    addr += n; buf += n; len -= n;
    yield();
  }
}

void FlashLogger::sectorErase(uint32_t addr, bool countErase) {
  writeEnable();
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_SE);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer(addr         & 0xFF);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
  delay(60);

  // VERIFY ERASE; quarantine if failed
  if (!verifyErase(addr)) {
    int s = (int)(addr / SECTOR_SIZE);
    Serial.printf("Erase verify FAILED on sector %d -> quarantine\n", s);
    quarantineSector(s);
    return;
  }

  if (countErase) {
    _factory.totalEraseOps++;
    saveFactoryInfo();
  }
}

// ===== sector/header helpers =====
bool FlashLogger::readSectorHeader(int sector, SectorHeader& hdr) {
  if (sector == FACTORY_SECTOR) return false;
  uint32_t base = sectorBaseAddr(sector);
  uint8_t buf[sizeof(SectorHeader)];
  readData(base, buf, sizeof(SectorHeader));
  memcpy(&hdr, buf, sizeof(SectorHeader));
  return (hdr.magic == 0x4C4F4747UL); // 'LOGG'
}

void FlashLogger::writeSectorHeader(int sector, uint16_t dayID, bool pushed) {
  SectorHeader hdr {};
  hdr.magic  = 0x4C4F4747UL;
  hdr.dayID  = dayID;
  hdr.pushed = pushed ? 1 : 0;
  hdr.reserved = 0;
  hdr.generation = _generation;
  pageProgram(sectorBaseAddr(sector), (const uint8_t*)&hdr, sizeof(SectorHeader));
  if (!verifyWrite(sectorBaseAddr(sector), (const uint8_t*)&hdr, sizeof(SectorHeader))) {
    Serial.printf("Header write verify FAILED on sector %d -> quarantine\n", sector);
    quarantineSector(sector);
  }
}

bool FlashLogger::sectorIsEmpty(int sector) {
  if (sector == FACTORY_SECTOR) return false;
  uint8_t b;
  readData(sectorBaseAddr(sector), &b, 1);
  return (b == 0xFF);
}

void FlashLogger::scanAllSectorsBuildIndex() {
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    SectorHeader hdr;
    if (readSectorHeader(s, hdr)) {
      _index[s].present = true;
      _index[s].dayID   = hdr.dayID;
      _index[s].pushed  = (hdr.pushed != 0);
      _index[s].writePtr = sectorBaseAddr(s) + sizeof(SectorHeader); // provisional
    }
  }
}

int FlashLogger::nextRoundRobinStart() {
  for (int attempts = 0; attempts < MAX_SECTORS; ++attempts) {
    uint16_t s = (_factory.startHint + attempts) % (MAX_SECTORS - 1); // [0..MAX-2]
    if (s == FACTORY_SECTOR) continue;
    if (!isBadSector(s)) {
      _factory.startHint = (s + 1) % (MAX_SECTORS - 1);
      saveFactoryInfo();
      return s;
    }
  }
  return 0;
}

void FlashLogger::selectOrCreateTodaySector() {
  int last = -1;
  for (int s = MAX_SECTORS - 1; s >= 0; --s) {
    if (s == FACTORY_SECTOR) continue;
    if (_index[s].present && _index[s].dayID == _currentDay && !isBadSector(s)) {
      last = s; break;
    }
  }
  if (last >= 0) { _currentSector = last; return; }

  int start = nextRoundRobinStart();
  for (int off = 0; off < MAX_SECTORS - 1; ++off) {
    int s = (start + off) % (MAX_SECTORS - 1);
    if (s == FACTORY_SECTOR) continue;
    if (isBadSector(s)) continue;
    if (sectorIsEmpty(s)) {
      sectorErase(sectorBaseAddr(s));
      writeSectorHeader(s, _currentDay, false);
      _index[s] = {true, _currentDay, false, sectorBaseAddr(s) + sizeof(SectorHeader)};
      _currentSector = s;
      return;
    }
    yield();
  }
  _currentSector = -1;
}

// header-aware rebuild to last valid record
void FlashLogger::findLastWritePositionInSector(int sector) {
  uint32_t base = sectorBaseAddr(sector);
  uint32_t ptr  = base + sizeof(SectorHeader);

  while (ptr + sizeof(RecordHeader) < base + SECTOR_SIZE) {
    RecordHeader rh;
    readData(ptr, (uint8_t*)&rh, sizeof(rh));
    if (rh.len == 0xFFFF || rh.len == 0x0000) break;
    if (ptr + sizeof(rh) + rh.len + 1 > base + SECTOR_SIZE) break;

    // commit byte
    uint8_t cm=0;
    readData(ptr + sizeof(rh) + rh.len, &cm, 1);
    if (cm != REC_COMMIT) break;

    // CRC check (no print)
    static uint8_t buf[PAGE_SIZE];
    uint32_t p = ptr + sizeof(rh);
    uint16_t remaining = rh.len;
    uint16_t crc = 0xFFFF;

    while (remaining) {
      uint16_t chunk = min<uint16_t>(remaining, PAGE_SIZE);
      readData(p, buf, chunk);
      crc = crc16(buf, chunk, crc);
      p += chunk;
      remaining -= chunk;
      yield();
    }
    if (crc != rh.crc) break;

    ptr += sizeof(rh) + rh.len + 1;
  }
  _index[sector].writePtr = ptr;
}

bool FlashLogger::sectorHasSpace(int sector, uint32_t needBytes) {
  uint32_t base = sectorBaseAddr(sector);
  uint32_t wp   = _index[sector].writePtr;
  return (wp + needBytes) <= (base + SECTOR_SIZE);
}

bool FlashLogger::moveToNextSectorSameDay() {
  for (int s = _currentSector + 1; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (isBadSector(s)) continue;
    if (sectorIsEmpty(s)) {
      sectorErase(sectorBaseAddr(s));
      writeSectorHeader(s, _currentDay, false);
      _index[s] = {true, _currentDay, false, sectorBaseAddr(s) + sizeof(SectorHeader)};
      _currentSector = s;
      _writeAddr = _index[s].writePtr;
      Serial.printf("Rolled to next sector %d for day %u\n", s, _currentDay);
      return true;
    }
    yield();
  }
  return false;
}

// ===== wear-leveling & quarantine =====
bool FlashLogger::isBadSector(int sector) const {
  for (int i = 0; i < _factory.badCount && i < 16; ++i)
    if (_factory.badList[i] == sector) return true;
  return false;
}

void FlashLogger::quarantineSector(int sector) {
  if (sector <= 0 || sector >= FACTORY_SECTOR) return;
  if (isBadSector(sector)) return;
  if (_factory.badCount < 16) {
    _factory.badList[_factory.badCount++] = sector;
    saveFactoryInfo();
  }
}

// ===== verify ops =====
bool FlashLogger::verifyErase(uint32_t base) {
  uint8_t buf[16];
  readData(base, buf, sizeof(buf));
  for (uint8_t b : buf) { if (b != 0xFF) return false; }
  return true;
}

bool FlashLogger::verifyWrite(uint32_t addr, const uint8_t* buf, uint32_t len) {
  uint8_t tmp[PAGE_SIZE];
  uint32_t off = 0;
  while (off < len) {
    uint32_t n = min<uint32_t>(PAGE_SIZE, len - off);
    readData(addr + off, tmp, n);
    if (memcmp(tmp, buf + off, n) != 0) return false;
    off += n;
    yield();
  }
  return true;
}

// ===== factory info helpers =====
bool FlashLogger::loadFactoryInfo() {
  uint8_t buf[sizeof(FactoryInfo)];
  readData(sectorBaseAddr(FACTORY_SECTOR), buf, sizeof(FactoryInfo));
  memcpy(&_factory, buf, sizeof(FactoryInfo));
  return (_factory.magic == 0x46414354UL);
}

void FlashLogger::saveFactoryInfo() {
  pageProgram(sectorBaseAddr(FACTORY_SECTOR), (const uint8_t*)&_factory, sizeof(FactoryInfo));
}

// ===== time helpers =====
uint16_t FlashLogger::dayIDFromDateTime(const DateTime& t) {
  return (uint16_t)(t.secondstime() / 86400UL);
}
bool FlashLogger::isOlderThanNDays(uint16_t baseDay, uint16_t targetDay, uint16_t n) {
  if (baseDay >= targetDay) return (baseDay - targetDay) >= n;
  return false;
}

// ===== printing helpers =====
void FlashLogger::formatDate(const DateTime& dt, char* out, size_t outLen) const {
  switch (_dateStyle) {
    case DATE_THAI: snprintf(out, outLen, "%02d/%02d/%02d", dt.day(), dt.month(), (dt.year()-2000)); break;
    case DATE_ISO:  snprintf(out, outLen, "%04d-%02d-%02d", dt.year(), dt.month(), dt.day()); break;
    case DATE_US:   snprintf(out, outLen, "%02d/%02d/%02d", dt.month(), dt.day(), (dt.year()-2000)); break;
    default:        snprintf(out, outLen, "%02d/%02d/%02d", dt.day(), dt.month(), (dt.year()-2000)); break;
  }
}

void FlashLogger::printSectorData(int sector) {
  uint32_t base = sectorBaseAddr(sector);
  uint32_t ptr  = base + sizeof(SectorHeader);

  while (ptr + sizeof(RecordHeader) < base + SECTOR_SIZE) {
    RecordHeader rh;
    readData(ptr, (uint8_t*)&rh, sizeof(rh));
    if (rh.len == 0xFFFF || rh.len == 0x0000) break;
    if (ptr + sizeof(rh) + rh.len + 1 > base + SECTOR_SIZE) break;

    // read payload & compute CRC while printing
    uint32_t p = ptr + sizeof(rh);
    uint16_t remaining = rh.len;
    uint16_t crc = 0xFFFF;
    static uint8_t buf[PAGE_SIZE];

    while (remaining) {
      uint16_t chunk = min<uint16_t>(remaining, PAGE_SIZE);
      readData(p, buf, chunk);
      crc = crc16(buf, chunk, crc);
      for (int i=0;i<chunk;i++) Serial.write(buf[i]); // prints newline too
      p += chunk;
      remaining -= chunk;
      yield();
    }

    uint8_t cm=0;
    readData(ptr + sizeof(rh) + rh.len, &cm, 1);
    if (cm != REC_COMMIT || crc != rh.crc) {
      Serial.println(F("[corrupt/partial record skipped]"));
      break;
    }

    ptr += sizeof(rh) + rh.len + 1;
  }
}

// ===== capacity / stats =====
uint32_t FlashLogger::countUsedSectors() const {
  uint32_t used = 0;
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (_index[s].present) used++;
  }
  return used;
}
float FlashLogger::getFreeSpaceMB() {
  float totalMB = (MAX_SECTORS - 1) * (SECTOR_SIZE / 1024.0f / 1024.0f);
  float usedMB  = countUsedSectors() * (SECTOR_SIZE / 1024.0f / 1024.0f);
  return max(0.0f, totalMB - usedMB);
}
float FlashLogger::getUsedSpaceMB() {
  return countUsedSectors() * (SECTOR_SIZE / 1024.0f / 1024.0f);
}
float FlashLogger::getUsedPercent() {
  float totalMB = (MAX_SECTORS - 1) * (SECTOR_SIZE / 1024.0f / 1024.0f);
  float usedMB  = getUsedSpaceMB();
  if (totalMB <= 0.0f) return 0.0f;
  return (usedMB / totalMB) * 100.0f;
}
float FlashLogger::getFlashHealth() {
  const float kCycles = 100000.0f;
  float avgCycles = (_factory.totalEraseOps) / max(1.0f, (float)(MAX_SECTORS - 1));
  float health = 1.0f - (avgCycles / kCycles);
  if (health < 0) health = 0;
  return health * 100.0f;
}
uint16_t FlashLogger::estimateDaysRemaining(float avgBytesPerDay) {
  if (avgBytesPerDay <= 0.0f) return 0;
  float freeBytes = getFreeSpaceMB() * 1024.0f * 1024.0f;
  float days = freeBytes / avgBytesPerDay;
  if (days < 0) days = 0;
  if (days > 65535) days = 65535;
  return (uint16_t)days;
}
FlashStats FlashLogger::getFlashStats(float avgBytesPerDay) {
  FlashStats s{};
  s.totalMB = (MAX_SECTORS - 1) * (SECTOR_SIZE / 1024.0f / 1024.0f);
  s.usedMB  = getUsedSpaceMB();
  s.freeMB  = max(0.0f, s.totalMB - s.usedMB);
  s.usedPercent = (s.totalMB > 0) ? (s.usedMB / s.totalMB) * 100.0f : 0.0f;
  s.healthPercent = getFlashHealth();
  s.estimatedDaysLeft = estimateDaysRemaining(avgBytesPerDay);
  return s;
}

// ===== factory info public =====
bool FlashLogger::setFactoryInfo(const String& model, const String& flashModel, const String& deviceID) {
  bool changed = false;
  if (_factory.magic != 0x46414354UL) return false;

  if (model.length())      { strncpy(_factory.model, model.c_str(), sizeof(_factory.model)-1); changed = true; }
  if (flashModel.length()) { strncpy(_factory.flashModel, flashModel.c_str(), sizeof(_factory.flashModel)-1); changed = true; }
  if (deviceID.length())   { strncpy(_factory.deviceId, deviceID.c_str(), sizeof(_factory.deviceId)-1); changed = true; }

  if (_factory.firstDayID == 0) { _factory.firstDayID = dayIDFromDateTime(_rtc->now()); changed = true; }
  _factory.defaultDateStyle = (uint8_t)_dateStyle;

  if (changed) saveFactoryInfo();
  return true;
}

void FlashLogger::printFactoryInfo() {
  Serial.println("=== Factory Info ===");
  Serial.printf("Model        : %s\n", _factory.model);
  Serial.printf("Flash Model  : %s\n", _factory.flashModel);
  Serial.printf("Device ID    : %s\n", _factory.deviceId);
  Serial.printf("First DayID  : %u\n", _factory.firstDayID);
  Serial.printf("Default Style: %u\n", (unsigned)_factory.defaultDateStyle);
  Serial.printf("Erase Ops    : %lu\n", (unsigned long)_factory.totalEraseOps);
  Serial.printf("Boot Count   : %lu\n", (unsigned long)_factory.bootCounter);
  Serial.printf("Bad Sectors  : %u\n", (unsigned)_factory.badCount);
  Serial.println("====================");
}

bool FlashLogger::factoryReset(const String& code12) {
  if (code12.length() != 12) return false;
  for (uint8_t c : code12) if (c < '0' || c > '9') return false;
  if (code12 != "847291506314") return false;

  Serial.println("FACTORY RESET: erasing all data sectors...");
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (_index[s].present || sectorIsEmpty(s) == false) {
      sectorErase(sectorBaseAddr(s)); // counts erase, verifies; may quarantine
      _index[s] = {false, 0, false, 0};
    }
    yield();
  }
  _currentDay = dayIDFromDateTime(_rtc->now());
  selectOrCreateTodaySector();
  if (_currentSector >= 0) {
    findLastWritePositionInSector(_currentSector);
    _writeAddr = _index[_currentSector].writePtr;
  }
  Serial.println("FACTORY RESET: done.");
  return true;
}

// ===== back-pressure =====
void FlashLogger::setMaxDailyBytes(uint32_t bytes) { _maxDailyBytes = bytes; }
bool FlashLogger::isLowSpace() const { return _lowSpace; }
