#include "FlashLogger.h"
#include <string.h>
#include <Preferences.h>

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

bool FlashLogger::begin(const FlashLoggerConfig& cfg) {
  _cfg = cfg;
  _rtc = cfg.rtc;
  if (!_rtc) { Serial.println("FlashLogger: RTC is required"); return false; }

  // SPI setup (respect custom pins if provided)
  // If your code already does SPI.begin() somewhere, keep that; else:
  if (cfg.spi_sck_pin >= 0 && cfg.spi_mosi_pin >= 0 && cfg.spi_miso_pin >= 0) {
    SPI.begin(cfg.spi_sck_pin, cfg.spi_miso_pin, cfg.spi_mosi_pin, cfg.spi_cs_pin);
  } else {
    SPI.begin(); // defaults
  }
  pinMode(cfg.spi_cs_pin, OUTPUT);
  digitalWrite(cfg.spi_cs_pin, HIGH);

  // flash init & scan (reuse your existing code)
  // - probe JEDEC, verify size, set total/sector sizes if you keep variables
  // - scanAllSectorsBuildIndex();
  // - selectOrCreateTodaySector();
  // - findLastWritePositionInSector(...);

  setDateStyle((uint8_t)cfg.dateStyle);
  setOutputFormat(cfg.defaultOut);
  setCsvColumns(cfg.csvColumns);

  // Set factory info once if empty (reuse your existing setFactoryInfo logic)
  setFactoryInfo(cfg.model, cfg.flashModel, cfg.deviceId);

  // Ready
  if (_cfg.enableShell) Serial.println("[FlashLogger] shell enabled (ls/cd/print/q/fmt/cursor/export/reset/gc/stats/factory)");
  return true;
}

void FlashLogger::setOutputFormat(LoggerOutFmt fmt) { _outFmt = fmt; }
void FlashLogger::setCsvColumns(const char* cols) {
  if (!cols || !*cols) return;
  strncpy(_csvCols, cols, sizeof(_csvCols)-1);
  _csvCols[sizeof(_csvCols)-1] = 0;
}

DateTime FlashLogger::nowRTC() const { return _rtc ? _rtc->now() : DateTime(2000,1,1,0,0,0); }
uint16_t FlashLogger::dayIDFromDateTime(const DateTime& dt) const {
  // keep your existing calculation (seconds since 2000 / 86400)
  return (uint16_t)(dt.secondstime() / 86400UL);
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

// ===== Sorting newest-first by dayID =====
int FlashLogger::cmpDayDesc(const void* a, const void* b) {
  const DaySummary* A = (const DaySummary*)a;
  const DaySummary* B = (const DaySummary*)b;
  if (A->dayID == B->dayID) return 0;
  return (A->dayID > B->dayID) ? -1 : 1;
}

// ===== DayID <-> date helpers =====
void FlashLogger::formatDayID(uint16_t dayID, char* out, size_t outLen) const {
  DateTime now = _rtc->now();
  int32_t delta = (int32_t)dayIDFromDateTime(now) - (int32_t)dayID;
  DateTime dt = now - TimeSpan(delta * 86400L);
  formatDate(dt, out, outLen);
}
bool FlashLogger::parseDateYYYYMMDD(const String& s, uint16_t& outDayID) {
  if (s.length() != 10 || s.charAt(4)!='-' || s.charAt(7)!='-') return false;
  int y = s.substring(0,4).toInt();
  int m = s.substring(5,7).toInt();
  int d = s.substring(8,10).toInt();
  if (y < 2000 || y > 2099 || m<1 || m>12 || d<1 || d>31) return false;
  DateTime dt(y,m,d,0,0,0);
  outDayID = (uint16_t)(dt.secondstime()/86400UL);
  return true;
}

// ===== valid-bytes scan (header-aware) =====
uint32_t FlashLogger::computeValidBytesInSector(int sector, uint32_t& firstTs, uint32_t& lastTs) {
  firstTs = 0; lastTs = 0;
  uint32_t base = sectorBaseAddr(sector);
  uint32_t ptr  = base + sizeof(SectorHeader);
  uint32_t bytes = 0;

  while (ptr + sizeof(RecordHeader) < base + SECTOR_SIZE) {
    RecordHeader rh;
    readData(ptr, (uint8_t*)&rh, sizeof(rh));
    if (rh.len == 0xFFFF || rh.len == 0x0000) break;
    if (ptr + sizeof(rh) + rh.len + 1 > base + SECTOR_SIZE) break;
    uint8_t cm=0;
    readData(ptr + sizeof(rh) + rh.len, &cm, 1);
    if (cm != REC_COMMIT) break;

    if (!firstTs) firstTs = rh.ts;
    lastTs = rh.ts;

    uint32_t rec = sizeof(RecordHeader) + rh.len + 1;
    bytes += rec;
    ptr   += rec;
    yield();
  }
  return bytes;
}

// ===== sector/day summaries =====
void FlashLogger::summarizeSector(int sector, SectorSummary& out) {
  out = {};
  SectorHeader hdr;
  if (!readSectorHeader(sector, hdr)) return;
  out.sector = sector;
  out.dayID  = hdr.dayID;
  out.pushed = (hdr.pushed != 0);
  out.bytes  = computeValidBytesInSector(sector, out.firstTs, out.lastTs);
}
void FlashLogger::summarizeDay(uint16_t dayID, DaySummary& out) {
  out = {};
  out.dayID = dayID;
  out.pushed = true;
  for (int s=0; s<MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    if (_index[s].dayID != dayID) continue;
    SectorSummary ss; summarizeSector(s, ss);
    out.sectors++;
    out.bytes += ss.bytes;
    if (!ss.pushed) out.pushed = false;
    if (!out.firstTs || (ss.firstTs && ss.firstTs < out.firstTs)) out.firstTs = ss.firstTs;
    if (ss.lastTs && ss.lastTs > out.lastTs) out.lastTs = ss.lastTs;
    yield();
  }
}

// ===== build cached day list =====
void FlashLogger::buildSummaries() {
  _dayCount = 0;
  uint16_t seen[MAX_DAYS_CACHE]; int seenN = 0;

  for (int s=0; s<MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    uint16_t d = _index[s].dayID;
    bool dup=false; for(int i=0;i<seenN;i++){ if (seen[i]==d){ dup=true; break; } }
    if (!dup && seenN < MAX_DAYS_CACHE) seen[seenN++] = d;
    yield();
  }
  for (int i=0;i<seenN;i++) {
    if (_dayCount >= MAX_DAYS_CACHE) break;
    summarizeDay(seen[i], _days[_dayCount++]);
    yield();
  }
  if (_dayCount > 1) qsort(_days, _dayCount, sizeof(DaySummary), cmpDayDesc);

  // refresh sector cache if a day is selected
  if (_selKind == SEL_DAY) listSectors(_selDay);
}

// ===== listings =====
void FlashLogger::listDays() {
  buildSummaries();
  Serial.println(F("\n#  DATE        DAYID  SECT  BYTES     STATUS  RANGE"));
  for (int i=0;i<_dayCount;i++) {
    char dateBuf[20]; formatDayID(_days[i].dayID, dateBuf, sizeof(dateBuf));
    const char* st = _days[i].pushed ? "PUSHED" : "OPEN";
    char rng[32]; snprintf(rng,sizeof(rng), "%lu..%lu", (unsigned long)_days[i].firstTs, (unsigned long)_days[i].lastTs);
    Serial.printf("%-2d %-10s  %-5u  %-4u  %-8lu  %-6s  %s\n",
      i, dateBuf, _days[i].dayID, _days[i].sectors, (unsigned long)_days[i].bytes, st, rng);
    yield();
  }
  if (_dayCount==0) Serial.println("(no days)");
}
void FlashLogger::listSectors(uint16_t dayID) {
  _sectCount = 0;
  for (int s=0; s<MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    if (_index[s].dayID != dayID) continue;
    if (_sectCount >= MAX_SECT_CACHE) break;
    summarizeSector(s, _sects[_sectCount++]);
    yield();
  }
  Serial.printf("\nSectors for day %u:\n", dayID);
  Serial.println(F("#  SECTOR  BYTES     STATUS   TS_RANGE"));
  for (int i=0;i<_sectCount;i++) {
    const char* st = _sects[i].pushed ? "PUSHED" : "OPEN";
    char rng[32]; snprintf(rng,sizeof(rng), "%lu..%lu", (unsigned long)_sects[i].firstTs, (unsigned long)_sects[i].lastTs);
    Serial.printf("%-2d %-6d  %-8lu %-7s  %s\n", i, _sects[i].sector, (unsigned long)_sects[i].bytes, st, rng);
    yield();
  }
  if (_sectCount==0) Serial.println("(no sectors)");
}

// ===== selection & print =====
bool FlashLogger::dayIndexToDayID(int idx, uint16_t& out) const {
  if (idx < 0 || idx >= _dayCount) return false; out = _days[idx].dayID; return true;
}
bool FlashLogger::sectorIndexToSector(int idx, int& out) const {
  if (idx < 0 || idx >= _sectCount) return false; out = _sects[idx].sector; return true;
}
bool FlashLogger::selectDay(uint16_t dayID) {
  _selKind = SEL_DAY; _selDay = dayID; _selSector = -1;
  listSectors(dayID);
  return true;
}
bool FlashLogger::selectSector(int sector) {
  if (_selKind == SEL_DAY) {
    SectorHeader hdr; if (!readSectorHeader(sector, hdr)) return false;
    if (hdr.dayID != _selDay) return false;
  }
  _selKind = SEL_SECTOR; _selSector = sector;
  return true;
}
void FlashLogger::printSelectedInfo() {
  if (_selKind == SEL_DAY) {
    DaySummary d; summarizeDay(_selDay, d);
    char dateBuf[20]; formatDayID(d.dayID, dateBuf, sizeof(dateBuf));
    Serial.printf("[DAY] %s  dayID=%u  sectors=%u  bytes=%lu  status=%s\n",
      dateBuf, d.dayID, d.sectors, (unsigned long)d.bytes, d.pushed?"PUSHED":"OPEN");
  } else if (_selKind == SEL_SECTOR) {
    SectorSummary s; summarizeSector(_selSector, s);
    Serial.printf("[SECTOR] #%d  dayID=%u  bytes=%lu  status=%s  ts=%lu..%lu\n",
      s.sector, s.dayID, (unsigned long)s.bytes, s.pushed?"PUSHED":"OPEN",
      (unsigned long)s.firstTs, (unsigned long)s.lastTs);
  } else {
    Serial.println("(nothing selected)");
  }
}
void FlashLogger::printSelected() {
  if (_selKind == SEL_DAY) {
    uint16_t day = _selDay;
    Serial.println("----------------------------------");
    char dateBuf[20]; formatDayID(day, dateBuf, sizeof(dateBuf));
    Serial.printf("date %s\n{\n", dateBuf);
    for (int s=0; s<MAX_SECTORS; ++s) {
      if (s == FACTORY_SECTOR) continue;
      if (!_index[s].present) continue;
      if (_index[s].dayID != day) continue;
      printSectorData(s);
      yield();
    }
    Serial.println("}\n----------------------------------");
  } else if (_selKind == SEL_SECTOR) {
    Serial.printf("\n[PRINT sector %d]\n", _selSector);
    printSectorData(_selSector);
    Serial.println("----------------------------------");
  } else {
    Serial.println("(nothing selected)");
  }
}
bool FlashLogger::handleCommand(const String& cmdIn, Stream& io) {
  String cmd = cmdIn; cmd.trim();
  if (!cmd.length()) return false;

  // reset (factory wipe + reinit)
  if (cmd.startsWith("reset")) {
    String arg = cmd.substring(5); arg.trim();
    if (!arg.length()) { io.println("⚠️  reset <12digit_code> (erases all logs)"); return true; }
    if (arg.length()!=12) { io.println("reset: code must be 12 digits"); return true; }
    for (uint8_t i=0;i<12;i++) if (arg[i]<'0'||arg[i]>'9'){ io.println("reset: numeric only"); return true; }
    if (factoryReset(arg)) { io.println("✅ reset OK, reinit..."); reinitAfterFactoryReset(); printFactoryInfo(); listDays(); }
    else io.println("❌ reset failed");
    return true;
  }

  // ls (days)
  if (cmd.equalsIgnoreCase("ls")) { listDays(); return true; }

  // ls sectors [dayID|YYYY-MM-DD]
  if (cmd.startsWith("ls sectors")) {
    uint16_t dayID = 0;
    int p = cmd.indexOf(' ', 3); p = (p>=0)?cmd.indexOf(' ', p+1):-1;
    if (p>0) {
      String a = cmd.substring(p+1); a.trim();
      if (a.length()==10 && a[4]=='-' && a[7]=='-') { if (!parseDateYYYYMMDD(a, dayID)) dayID=0; }
      else if (a.length()) dayID = (uint16_t)a.toInt();
    }
    if (!dayID) { buildSummaries(); if (!_dayCount){ io.println("(no days)"); return true; } dayID=_days[0].dayID; }
    listSectors(dayID); return true;
  }

  // cd day <YYYY-MM-DD | dayID | #index>
  if (cmd.startsWith("cd day")) {
    String a = cmd.substring(6); a.trim(); if (!a.length()){ io.println("usage: cd day <YYYY-MM-DD|dayID|#index>"); return true; }
    uint16_t dayID=0;
    if (a.startsWith("#")) { int idx=a.substring(1).toInt(); buildSummaries(); if (!dayIndexToDayID(idx, dayID)){ io.println("invalid day index"); return true; } }
    else if (a.length()==10 && a[4]=='-' && a[7]=='-') { if (!parseDateYYYYMMDD(a, dayID)){ io.println("bad date"); return true; } }
    else { dayID=(uint16_t)a.toInt(); if (!dayID){ buildSummaries(); if (!_dayCount){ io.println("(no days)"); return true; } dayID=_days[0].dayID; } }
    selectDay(dayID); printSelectedInfo(); return true;
  }

  // cd sector <sectorID | #index>
  if (cmd.startsWith("cd sector")) {
    String a = cmd.substring(9); a.trim(); if (!a.length()){ io.println("usage: cd sector <sectorID|#index>"); return true; }
    int sector = a.startsWith("#") ? (sectorIndexToSector(a.substring(1).toInt(), sector), sector) : a.toInt();
    if (sector<0){ io.println("invalid sector id"); return true; }
    if (selectSector(sector)) printSelectedInfo(); else io.println("sector not in selected day");
    return true;
  }

  // print/info
  if (cmd.equalsIgnoreCase("print")) { printSelected(); return true; }
  if (cmd.equalsIgnoreCase("info"))  { printSelectedInfo(); return true; }

  // queries / format / csv columns
  if (cmd.startsWith("q "))        { if (handleQueryCommand(cmd, io)) return true; }
  if (cmd.startsWith("fmt "))      { String a=cmd.substring(4); a.trim();
                                     if (a.equalsIgnoreCase("csv"))   { setOutputFormat(OUT_CSV);   io.println("format: CSV"); return true; }
                                     if (a.equalsIgnoreCase("jsonl")) { setOutputFormat(OUT_JSONL); io.println("format: JSONL"); return true; }
                                     io.println("fmt csv|jsonl"); return true; }
  if (cmd.startsWith("set csv "))  { String cols=cmd.substring(8); cols.trim(); setCsvColumns(cols.c_str());
                                     io.print("csv columns: "); io.println(getCsvColumns()); return true; }

  // cursor / export
  if (cmd.startsWith("cursor") || cmd.startsWith("export")) {
    if (handleCursorCommand(cmd, io)) return true;
  }

  // built-in extras
  if (cmd.equalsIgnoreCase("pf"))     { printFormattedLogs(); return true; }
  if (cmd.equalsIgnoreCase("stats"))  { auto fs=getFlashStats(3500.0f);
    io.printf("Total: %.2f MB  Used: %.2f MB  Free: %.2f MB  Used: %.1f%%  Health: %.1f%%  EstDays: %u\n",
              fs.totalMB, fs.usedMB, fs.freeMB, fs.usedPercent, fs.healthPercent, fs.estimatedDaysLeft); return true; }
  if (cmd.equalsIgnoreCase("factory")){ printFactoryInfo(); return true; }
  if (cmd.equalsIgnoreCase("gc"))     { gc(); return true; }

    // cursor save [ns key]
  if (cmd.startsWith("cursor save")) {
    String rest = cmd.substring(11); rest.trim();
    String ns="flog", key="cursor";
    if (rest.length()) {
      int sp = rest.indexOf(' ');
      if (sp<0) key = rest; else { ns = rest.substring(0,sp); key = rest.substring(sp+1); ns.trim(); key.trim(); }
    }
    bool ok = saveCursorNVS(ns.c_str(), key.c_str());
    io.println(ok ? "cursor: saved" : "cursor: save failed");
    return true;
  }
  // cursor load [ns key]
  if (cmd.startsWith("cursor load")) {
    String rest = cmd.substring(11); rest.trim();
    String ns="flog", key="cursor";
    if (rest.length()) {
      int sp = rest.indexOf(' ');
      if (sp<0) key = rest; else { ns = rest.substring(0,sp); key = rest.substring(sp+1); ns.trim(); key.trim(); }
    }
    bool ok = loadCursorNVS(ns.c_str(), key.c_str());
    io.println(ok ? "cursor: loaded" : "cursor: load failed");
    return true;
  }

  if (cmd.equalsIgnoreCase("scanbad")) { scanBadAndQuarantine(io); return true; }

  return false; // let caller handle anything else
}



//1.92
void FlashLogger::setOutputFormat(OutFmt f) { _outFmt = f; }
void FlashLogger::setCsvColumns(const char* cols_csv) {
  if (!cols_csv || !*cols_csv) return;
  strncpy(_csvCols, cols_csv, sizeof(_csvCols)-1);
  _csvCols[sizeof(_csvCols)-1] = 0;
}
bool FlashLogger::recordMatchesTime(uint16_t recDay, uint32_t ts, const QuerySpec& q) {
  // Day range overrides ts range if set
  if (q.day_from || q.day_to) {
    if (q.day_from && recDay < q.day_from) return false;
    if (q.day_to   && recDay > q.day_to)   return false;
    return true;
  }
  if (ts < q.ts_from || ts > q.ts_to) return false;
  return true;
}
bool FlashLogger::jsonExtractKeyValue(const char* json, const char* key, String& outVal) const {
  // Find "key":
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p && (*p==' ' || *p=='\t' || *p==':')) p++;
  if (!*p) return false;

  // Number or string
  if (*p == '\"') {
    const char* q = ++p;
    while (*q && *q != '\"') q++;
    outVal = String("\"") + String(p, q-p) + String("\"");
    return true;
  } else {
    const char* q = p;
    while (*q && *q!='\n' && *q!='\r' && *q!=',' && *q!='}') q++;
    outVal = String(p, q-p);
    outVal.trim();
    return outVal.length() > 0;
  }
}

void FlashLogger::buildCsvLine(uint32_t ts, const char* payload, uint16_t len, const char* cols, String& out) {
  out = "";
  // simple CSV: split cols by comma
  String colsS(cols);
  colsS.trim();
  int start = 0;
  bool first = true;
  while (start < colsS.length()) {
    int comma = colsS.indexOf(',', start);
    String col = (comma >= 0) ? colsS.substring(start, comma) : colsS.substring(start);
    col.trim();
    String val;
    if (col.equalsIgnoreCase("ts")) {
      val = String(ts);
    } else {
      // pull from JSON
      jsonExtractKeyValue(payload, col.c_str(), val);
      // remove quotes around strings for CSV cleanliness
      if (val.startsWith("\"") && val.endsWith("\"")) {
        val = val.substring(1, val.length()-1);
      }
    }
    if (!first) out += ",";
    out += val.length() ? val : "";
    first = false;
    if (comma < 0) break;
    start = comma + 1;
  }
}

void FlashLogger::buildJsonFiltered(const char* payload, uint16_t len,
                                    const char* const* keys, bool compact, String& out) {
  if (!keys || !keys[0]) {
    // no filter → pass-through (ensure newline)
    out = String(payload, len);
    if (out.length()==0 || out[out.length()-1] != '\n') out += '\n';
    return;
  }
  // build {"k1":v1,"k2":v2,...}
  out = "{";
  bool first = true;
  for (int i=0; i<8 && keys[i]; ++i) {
    String v;
    if (jsonExtractKeyValue(payload, keys[i], v)) {
      if (!first) out += ",";
      out += "\""; out += keys[i]; out += "\":";
      out += v;
      first = false;
    }
  }
  out += "}\n";
  if (!compact) {
    // (optional pretty — keeping compact for now)
  }
}
bool FlashLogger::emitRecord(uint16_t recDay, uint32_t ts, const uint8_t* payload, uint16_t len,
                             const QuerySpec& q, RowCallback onRow, void* user) {
  if (!onRow) return false;
  static String line;
  if (q.out == OUT_JSONL) {
    buildJsonFiltered((const char*)payload, len, q.includeKeys, q.compact_json, line);
  } else { // CSV
    buildCsvLine(ts, (const char*)payload, len, _csvCols, line);
    line += "\n";
  }
  onRow(line.c_str(), user);
  return true;
}
uint32_t FlashLogger::queryLogs(const QuerySpec& q, RowCallback onRow, void* user) {
  if (!onRow) return 0;

  uint32_t emitted = 0;
  uint32_t sample  = 0;

  // Build anchors once (simple guard; you can rebuild after reindex if needed)
  static bool anchorsBuilt = false;
  if (!anchorsBuilt) { buildAnchors(); anchorsBuilt = true; }

  // Sector scan with anchor prefilter
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present)  continue;

    // quick skip if sector cannot overlap the requested window
    if (!sectorMaybeInRangeByAnchor(s, q.day_from, q.day_to, q.ts_from, q.ts_to)) {
      continue;
    }

    const uint32_t base = sectorBaseAddr(s);
    uint32_t ptr = base + sizeof(SectorHeader);

    while (ptr + sizeof(RecordHeader) < base + SECTOR_SIZE) {
      RecordHeader rh; uint16_t recDay;
      if (!readRecordMeta(ptr, rh, recDay)) {
        // hit first gap/invalid -> end of valid records in this sector
        break;
      }

      // time window filter
      if (recordMatchesTime(recDay, rh.ts, q)) {
        // sampling
        if (q.sample_every <= 1 || (sample++ % q.sample_every == 0)) {
          // read payload in PAGE_SIZE chunks
          static uint8_t buf[PAGE_SIZE];
          String payload; payload.reserve(rh.len + 8);
          uint32_t p = ptr + sizeof(rh);
          uint16_t remaining = rh.len;

          while (remaining) {
            uint16_t chunk = (remaining > PAGE_SIZE) ? PAGE_SIZE : remaining;
            readData(p, buf, chunk);
            payload += String((const char*)buf, chunk);
            p += chunk; remaining -= chunk;
            yield();
          }

          // emit as JSONL/CSV according to q
          if (emitRecord(recDay, rh.ts, (const uint8_t*)payload.c_str(), rh.len, q, onRow, user)) {
            ++emitted;
            if (q.max_records && emitted >= q.max_records) return emitted;
          }
        }
      }

      // advance to next record
      ptr += sizeof(rh) + rh.len + 1; // header + payload + commit byte
      yield();
    }
  }

  return emitted;
}

uint32_t FlashLogger::queryLatest(uint32_t N, RowCallback onRow, void* user) {
  if (N == 0) return 0;
  // simple approach: scan forward, keep only last N via max_records
  QuerySpec q; q.max_records = N; q.out = _outFmt; // inherit format
  return queryLogs(q, onRow, user); // (for huge logs, a reverse scan can be added later)
}

uint32_t FlashLogger::queryRange(uint32_t ts_from, uint32_t ts_to, RowCallback onRow, void* user) {
  QuerySpec q; q.ts_from = ts_from; q.ts_to = ts_to; q.out = _outFmt;
  return queryLogs(q, onRow, user);
}

uint32_t FlashLogger::queryBattery(RowCallback onRow, void* user) {
  QuerySpec q; q.out = _outFmt;
  q.includeKeys[0] = "bat"; q.includeKeys[1] = nullptr;
  return queryLogs(q, onRow, user);
}

bool FlashLogger::handleQueryCommand(const String& cmd, Stream& io) {
  // Patterns:
  // q latest <N> [keys...]
  // q day <YYYY-MM-DD> [keys...]
  // q range <YYYY-MM-DD>..<YYYY-MM-DD> [keys...]
  // Optional keys → filter fields in JSON; CSV ignores keys list and uses setCsvColumns()

  QuerySpec q; q.out = _outFmt; q.compact_json = true;

  // extract arguments
  String rest = cmd.substring(2); rest.trim(); // after "q "
  if (rest.startsWith("latest")) {
    rest.remove(0, 6); rest.trim();
    int sp = rest.indexOf(' ');
    String nStr = (sp>=0) ? rest.substring(0, sp) : rest;
    uint32_t N = nStr.toInt();
    // keys
    int k = 0;
    if (sp >= 0) {
      String keys = rest.substring(sp+1); keys.trim();
      int pos=0;
      while (k < 7 && pos < keys.length()) {
        int sp2 = keys.indexOf(' ', pos);
        String key = (sp2>=0) ? keys.substring(pos, sp2) : keys.substring(pos);
        key.trim();
        if (key.length()) q.includeKeys[k++] = strdup(key.c_str());
        if (sp2 < 0) break;
        pos = sp2+1;
      }
      q.includeKeys[k] = nullptr;
    }
    uint32_t outCount = queryLatest(N ? N : 100, [](const char* line, void* u){ ((Stream*)u)->print(line); }, &io);
    io.printf("(%lu rows)\n", (unsigned long)outCount);
    return true;
  }

  if (rest.startsWith("day ")) {
    String date = rest.substring(4); date.trim();
    // split date & optional keys
    String keys; int sp = date.indexOf(' ');
    if (sp>=0) { keys = date.substring(sp+1); date = date.substring(0, sp); date.trim(); keys.trim(); }
    uint16_t dFrom=0; if (!parseDateYYYYMMDD(date, dFrom)) { io.println("bad date (YYYY-MM-DD)"); return true; }
    q.day_from = dFrom; q.day_to = dFrom;

    // keys
    int k=0;
    while (keys.length() && k<7) {
      int sp2 = keys.indexOf(' ');
      String key = (sp2>=0) ? keys.substring(0, sp2) : keys;
      key.trim(); if (key.length()) q.includeKeys[k++] = strdup(key.c_str());
      if (sp2<0) break; keys = keys.substring(sp2+1); keys.trim();
    }
    q.includeKeys[k] = nullptr;

    uint32_t outCount = queryLogs(q, [](const char* line, void* u){ ((Stream*)u)->print(line); }, &io);
    io.printf("(%lu rows)\n", (unsigned long)outCount);
    return true;
  }

  if (rest.startsWith("range ")) {
    String rr = rest.substring(6); rr.trim();
    // "YYYY-MM-DD..YYYY-MM-DD" [keys...]
    String keys; int sp = rr.indexOf(' ');
    if (sp>=0) { keys = rr.substring(sp+1); rr = rr.substring(0, sp); rr.trim(); keys.trim(); }
    int dots = rr.indexOf("..");
    if (dots < 0) { io.println("range format: YYYY-MM-DD..YYYY-MM-DD"); return true; }
    String d1 = rr.substring(0, dots);
    String d2 = rr.substring(dots+2);
    uint16_t D1=0, D2=0;
    if (!parseDateYYYYMMDD(d1, D1) || !parseDateYYYYMMDD(d2, D2)) { io.println("bad date(s)"); return true; }
    q.day_from = min(D1, D2); q.day_to = max(D1, D2);

    int k=0;
    while (keys.length() && k<7) {
      int sp2 = keys.indexOf(' ');
      String key = (sp2>=0) ? keys.substring(0, sp2) : keys;
      key.trim(); if (key.length()) q.includeKeys[k++] = strdup(key.c_str());
      if (sp2<0) break; keys = keys.substring(sp2+1); keys.trim();
    }
    q.includeKeys[k] = nullptr;

    uint32_t outCount = queryLogs(q, [](const char* line, void* u){ ((Stream*)u)->print(line); }, &io);
    io.printf("(%lu rows)\n", (unsigned long)outCount);
    return true;
  }

  io.println("q latest <N> [keys...]");
  io.println("q day <YYYY-MM-DD> [keys...]");
  io.println("q range <YYYY-MM-DD>..<YYYY-MM-DD> [keys...]");
  return true;
}

bool FlashLogger::readRecordMeta(uint32_t addr, RecordHeader& rh, uint16_t& recDay) const {
  // determine sector & day from address
  int sector = addr / SECTOR_SIZE;
  if (sector < 0 || sector >= MAX_SECTORS || sector == FACTORY_SECTOR) return false;
  SectorHeader sh;
  if (!((FlashLogger*)this)->readSectorHeader(sector, sh)) return false;
  recDay = sh.dayID;

  // bounds
  uint32_t base = sectorBaseAddr(sector);
  if (addr < base + sizeof(SectorHeader) || addr + sizeof(RecordHeader) >= base + SECTOR_SIZE) return false;

  ((FlashLogger*)this)->readData(addr, (uint8_t*)&rh, sizeof(rh));
  if (rh.len == 0xFFFF || rh.len == 0x0000) return false;
  if (addr + sizeof(rh) + rh.len + 1 > base + SECTOR_SIZE) return false;

  // commit byte quick-check
  uint8_t cm=0; ((FlashLogger*)this)->readData(addr + sizeof(rh) + rh.len, &cm, 1);
  if (cm != REC_COMMIT) return false;

  return true;
}

bool FlashLogger::isValidRecordAt(uint32_t addr) const {
  RecordHeader rh; uint16_t d;
  return readRecordMeta(addr, rh, d);
}

bool FlashLogger::findFirstRecord(int sector, uint32_t& outAddr) {
  if (sector < 0 || sector >= MAX_SECTORS || sector == FACTORY_SECTOR) return false;
  if (!_index[sector].present) return false;
  uint32_t base = sectorBaseAddr(sector);
  uint32_t ptr  = base + sizeof(SectorHeader);

  while (ptr + sizeof(RecordHeader) < base + SECTOR_SIZE) {
    if (isValidRecordAt(ptr)) { outAddr = ptr; return true; }
    // if header not valid, break (we are at first gap)
    RecordHeader rh; uint16_t d;
    if (!readRecordMeta(ptr, rh, d)) break;
    ptr += sizeof(rh) + rh.len + 1;
    yield();
  }
  return false;
}

bool FlashLogger::findNextRecordAddr(int sector, uint32_t curAddr, uint32_t& nextAddr) {
  if (sector < 0 || sector >= MAX_SECTORS || sector == FACTORY_SECTOR) return false;
  uint32_t base = sectorBaseAddr(sector);
  if (curAddr < base + sizeof(SectorHeader)) return false;

  RecordHeader rh; uint16_t d;
  if (!readRecordMeta(curAddr, rh, d)) return false;

  uint32_t ptr = curAddr + sizeof(rh) + rh.len + 1;
  if (ptr + sizeof(RecordHeader) >= base + SECTOR_SIZE) return false;

  if (isValidRecordAt(ptr)) { nextAddr = ptr; return true; }
  // allow fast break if next header invalid
  return false;
}

bool FlashLogger::earliestCursor(SyncCursor& out) const {
  // scan sectors in order, pick first with a valid record
  for (int s=0; s<MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    uint32_t addr;
    if (((FlashLogger*)this)->findFirstRecord(s, addr)) {
      out.dayID  = _index[s].dayID;
      out.sector = s;
      out.addr   = addr;
      out.seq_next = 0; // unknown; reader doesn’t require it
      return true;
    }
    yield();
  }
  return false;
}

bool FlashLogger::advanceToNextValid(SyncCursor& c) const {
  if (c.sector < 0) return false;
  uint32_t nxt;
  if (findNextRecordAddr(c.sector, c.addr, nxt)) {
    c.addr = nxt;
    return true;
  }
  // move to first record of next sector (same day or later)
  for (int s=c.sector+1; s<MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    uint32_t addr;
    if (((FlashLogger*)this)->findFirstRecord(s, addr)) {
      c.dayID  = _index[s].dayID;
      c.sector = s;
      c.addr   = addr;
      return true;
    }
    yield();
  }
  return false;
}

bool FlashLogger::getCursor(SyncCursor& out) const {
  // “bookmark now”: point to NEXT record that would be written
  out.dayID  = _currentDay;
  out.sector = _currentSector;
  out.addr   = _index[_currentSector].writePtr; // points at empty space (next header)
  out.seq_next = _seqCounter;
  return true;
}

bool FlashLogger::setCursor(const SyncCursor& in) {
  // Validate: allow two styles
  // 1) A real record header address → use it
  // 2) A sector’s first record if addr==0 → resolve automatically
  SyncCursor c = in;

  if (c.sector < 0 || c.sector >= MAX_SECTORS || c.sector == FACTORY_SECTOR) return false;
  if (!_index[c.sector].present) return false;

  if (c.addr == 0) {
    if (!findFirstRecord(c.sector, c.addr)) return false;
  } else if (!isValidRecordAt(c.addr)) {
    // If it points to writePtr (future), bump to next existing sector with data
    uint32_t wp = _index[c.sector].writePtr;
    if (c.addr == wp) {
      if (!advanceToNextValid(c)) return false;
    } else {
      return false;
    }
  }
  _readCursor = c;
  return true;
}

void FlashLogger::clearCursor() {
  SyncCursor c{};
  if (earliestCursor(c)) _readCursor = c;
  else _readCursor = {0,-1,0,0};
}

uint32_t FlashLogger::exportSince(const SyncCursor& from, uint32_t max_rows,
                                  RowCallback onRow, void* user) {
  if (!onRow) return 0;

  // start position (validated copy)
  SyncCursor cur = from;
  if (cur.sector < 0 || !isValidRecordAt(cur.addr)) {
    // try to normalize via setCursor-like logic
    if (!setCursor(from)) return 0;
    cur = _readCursor;
  }

  uint32_t emitted = 0;

  while (true) {
    // read meta
    RecordHeader rh; uint16_t recDay;
    if (!readRecordMeta(cur.addr, rh, recDay)) {
      if (!advanceToNextValid(cur)) break;
      continue;
    }

    // read payload
    static uint8_t buf[PAGE_SIZE];
    String payload;
    uint32_t p = cur.addr + sizeof(rh);
    uint16_t remaining = rh.len;
    while (remaining) {
      uint16_t chunk = min<uint16_t>(remaining, PAGE_SIZE);
      readData(p, buf, chunk);
      payload += String((const char*)buf, chunk);
      p += chunk; remaining -= chunk; yield();
    }

    // emit row in current output format
    QuerySpec q; q.out = _outFmt; q.compact_json = true;
    emitRecord(recDay, rh.ts, (const uint8_t*)payload.c_str(), rh.len, q, onRow, user);

    emitted++;
    if (max_rows && emitted >= max_rows) break;

    if (!advanceToNextValid(cur)) break;
  }

  return emitted;
}

void FlashLogger::markDaysPushedUntil(uint16_t dayID_inclusive) {
  for (int s=0; s<MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    if (_index[s].dayID <= dayID_inclusive) {
      _index[s].pushed = true;
      SectorHeader hdr;
      if (readSectorHeader(s, hdr)) {
        if (hdr.pushed == 0) {
          hdr.pushed = 1;
          pageProgram(sectorBaseAddr(s), (const uint8_t*)&hdr, sizeof(SectorHeader));
          verifyWrite(sectorBaseAddr(s), (const uint8_t*)&hdr, sizeof(SectorHeader));
        }
      }
    }
    yield();
  }
  // io_flush(); // optional: if you have any buffered IO, otherwise delete
}

bool FlashLogger::handleCursorCommand(const String& cmd, Stream& io) {
  // cursor show | clear | set <dayID> <sector> <addr> <seq>
  // export <max_rows>
  String s = cmd; s.trim();

  if (s.equalsIgnoreCase("cursor show")) {
    io.printf("cursor: day=%u sector=%d addr=0x%06lX seq=%lu\n",
              _readCursor.dayID, _readCursor.sector, (unsigned long)_readCursor.addr,
              (unsigned long)_readCursor.seq_next);
    return true;
  }
  if (s.equalsIgnoreCase("cursor clear")) {
    clearCursor();
    io.println("cursor: reset to earliest");
    return true;
  }
  if (s.startsWith("cursor set")) {
    // cursor set <dayID> <sector> <addr> <seq>
    String rest = s.substring(10); rest.trim();
    int sp1 = rest.indexOf(' '); if (sp1<0) { io.println("usage: cursor set <dayID> <sector> <addr> <seq>"); return true; }
    int sp2 = rest.indexOf(' ', sp1+1); if (sp2<0) { io.println("usage: cursor set <dayID> <sector> <addr> <seq>"); return true; }
    int sp3 = rest.indexOf(' ', sp2+1); if (sp3<0) { io.println("usage: cursor set <dayID> <sector> <addr> <seq>"); return true; }
    uint16_t day = (uint16_t)rest.substring(0, sp1).toInt();
    int      sec = rest.substring(sp1+1, sp2).toInt();
    uint32_t adr = (uint32_t)strtoul(rest.substring(sp2+1, sp3).c_str(), nullptr, 0); // allow 0x...
    uint32_t seq = (uint32_t)rest.substring(sp3+1).toInt();

    SyncCursor c{day, sec, adr, seq};
    if (setCursor(c)) io.println("cursor: set OK");
    else io.println("cursor: set FAILED");
    return true;
  }
  if (s.startsWith("export")) {
    // export <max_rows>
    String rest = s.substring(6); rest.trim();
    uint32_t N = rest.length() ? (uint32_t)rest.toInt() : 0;
    uint32_t rows = exportSince(_readCursor, N, [](const char* line, void* u){ ((Stream*)u)->print(line); }, &io);
    io.printf("(%lu rows)\n", (unsigned long)rows);
    return true;
  }

  return false;
}

void FlashLogger::reinitAfterFactoryReset() {
  // rebuild indexes and pick a fresh sector for today
  for (int i = 0; i < MAX_SECTORS; ++i) {
    _index[i] = {false, 0, false, 0};
  }
  scanAllSectorsBuildIndex();

  DateTime now = _rtc->now();
  _currentDay = dayIDFromDateTime(now);
  selectOrCreateTodaySector();
  if (_currentSector >= 0) {
    findLastWritePositionInSector(_currentSector);
    _writeAddr = _index[_currentSector].writePtr;
  } else {
    Serial.println("FlashLogger: no sector available after reset!");
  }

  // clear navigation caches/selection
  _dayCount = 0; _sectCount = 0;
  _selKind = SEL_NONE; _selDay = 0; _selSector = -1;
  _seqCounter = 0;
  _todayBytes = 0;
  _lowSpace = false;
}

bool FlashLogger::saveCursorNVS(const char* ns, const char* key) {
  Preferences p;
  if (!p.begin(ns, false)) return false;
  bool ok = true;
  ok &= p.putUShort(String(key)+"_day", _readCursor.dayID);
  ok &= p.putInt   (String(key)+"_sec", _readCursor.sector) >= 0;
  ok &= p.putUInt  (String(key)+"_adr", _readCursor.addr);
  ok &= p.putUInt  (String(key)+"_seq", _readCursor.seq_next);
  p.end();
  return ok;
}

bool FlashLogger::loadCursorNVS(SyncCursor& out, const char* ns, const char* key) {
  Preferences p;
  if (!p.begin(ns, true)) return false;
  out.dayID   = p.getUShort(String(key)+"_day", 0);
  out.sector  = p.getInt   (String(key)+"_sec", -1);
  out.addr    = p.getUInt  (String(key)+"_adr", 0);
  out.seq_next= p.getUInt  (String(key)+"_seq", 0);
  p.end();
  // validate before returning
  return (out.sector >= 0) && isValidRecordAt(out.addr);
}

bool FlashLogger::firstRecordInSector(int sector, uint32_t& firstAddr, uint32_t& firstTs, uint32_t& lastTs) {
  if (sector < 0 || sector >= MAX_SECTORS || sector == FACTORY_SECTOR) return false;
  if (!_index[sector].present) return false;

  uint32_t base = sectorBaseAddr(sector);
  uint32_t ptr  = base + sizeof(SectorHeader);
  bool found = false;
  firstTs = 0; lastTs = 0;

  while (ptr + sizeof(RecordHeader) < base + SECTOR_SIZE) {
    RecordHeader rh; uint16_t d;
    if (!readRecordMeta(ptr, rh, d)) break;
    if (!found) {
      firstAddr = ptr;
      firstTs   = rh.ts;
      found = true;
    }
    lastTs = rh.ts;
    ptr += sizeof(rh) + rh.len + 1;
  }
  return found;
}

void FlashLogger::buildAnchors() {
  _anchorCount = 0;
  for (int s=0; s<MAX_SECTORS && _anchorCount<MAX_ANCHORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    if (!_index[s].present) continue;
    uint32_t fa, fts, lts;
    if (firstRecordInSector(s, fa, fts, lts)) {
      Anchor& a = _anchors[_anchorCount++];
      a.sector   = s;
      a.dayID    = _index[s].dayID;
      a.firstTs  = fts;
      a.firstAddr= fa;
      a.lastTs   = lts;
    }
    yield();
  }
}

bool FlashLogger::sectorMaybeInRangeByAnchor(int sector, uint16_t dayFrom, uint16_t dayTo,
                                             uint32_t ts_from, uint32_t ts_to) const {
  // if day range used, decide by day
  if (dayFrom || dayTo) {
    uint16_t d = _index[sector].dayID;
    if (dayFrom && d < dayFrom) return false;
    if (dayTo   && d > dayTo)   return false;
    return true;
  }
  // else use timestamp window from anchors
  for (int i=0;i<_anchorCount;++i) {
    if (_anchors[i].sector == sector) {
      // overlap check
      return !(_anchors[i].lastTs < ts_from || _anchors[i].firstTs > ts_to);
    }
  }
  // unknown → fall back to true
  return true;
}

void FlashLogger::scanBadAndQuarantine(Stream& io) {
  io.println("scanbad: scanning sectors...");
  int quarantined = 0;
  for (int s=0; s<MAX_SECTORS; ++s) {
    if (s == FACTORY_SECTOR) continue;
    uint32_t base = sectorBaseAddr(s);

    // Heuristic: try to read header; if magic is corrupt or repeated write/erase fails verification,
    // quarantine the sector. We DO NOT erase during scan—non-destructive.
    SectorHeader sh;
    if (!readSectorHeader(s, sh)) {
      io.printf("  sector %d: unreadable header → quarantine\n", s);
      quarantineSector(s); quarantined++; continue;
    }

    // Optional lightweight write/verify probe at end of free area (skip if present day)
    // We keep it passive by default to avoid wearing the flash.
    // (Leave hooks here if you want an active probe mode later.)
    yield();
  }
  io.printf("scanbad: done. quarantined=%d\n", quarantined);
}
