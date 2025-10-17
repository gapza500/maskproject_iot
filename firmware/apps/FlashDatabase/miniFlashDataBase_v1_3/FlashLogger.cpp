#include "FlashLogger.h"

// ====== ctor ======
FlashLogger::FlashLogger(uint8_t csPin) : _cs(csPin) {}

// ====== begin ======
bool FlashLogger::begin(RTC_DS3231* rtc) {
  _rtc = rtc;

  // SPI + CS
  SPI.begin(23, 21, 22, _cs);
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  delay(5);

  // init RAM index
  for (int i = 0; i < MAX_SECTORS; ++i) {
    _index[i] = {false, 0, false, 0};
  }

  // Build index from headers already in flash
  scanAllSectorsBuildIndex();

  // Today
  DateTime now = _rtc->now();
  _currentDay = dayIDFromDateTime(now);

  // Choose a sector for today (existing chain or new)
  selectOrCreateTodaySector();

  // Find the exact write pointer inside current sector
  if (_currentSector >= 0) {
    findLastWritePositionInSector(_currentSector);
    _writeAddr = _index[_currentSector].writePtr;
  } else {
    // Couldn't find/allocate sector (shouldn't happen on 8MB)
    Serial.println("FlashLogger: no sector available!");
    return false;
  }

  Serial.printf("FlashLogger v1.4 ready. Day=%u Sector=%d NextWrite=0x%06lX\n",
                _currentDay, _currentSector, _writeAddr);
  return true;
}

// ====== append ======
bool FlashLogger::append(const String& json) {
  if (_currentSector < 0) return false;

  const uint16_t len = min<uint16_t>(json.length() + 1, PAGE_SIZE);

  // If not enough space in current sector, roll to next sector for the same day
  if (!sectorHasSpace(_currentSector, len)) {
    if (!moveToNextSectorSameDay()) {
      Serial.println("FlashLogger: Out of sectors for today, append aborted.");
      return false;
    }
  }

  // Write the bytes
  Serial.printf("APPEND @0x%06lX (sector %d) len=%u\n", _writeAddr, _currentSector, len);
  writeEnable();
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_PP);
  SPI.transfer((_writeAddr >> 16) & 0xFF);
  SPI.transfer((_writeAddr >> 8)  & 0xFF);
  SPI.transfer(_writeAddr         & 0xFF);
  for (uint16_t i = 0; i < len; ++i) SPI.transfer((uint8_t)json[i]);
  digitalWrite(_cs, HIGH);
  delay(3);

  // Advance pointers
  _writeAddr += len;
  _index[_currentSector].writePtr = _writeAddr;
  return true;
}

// ====== printFormattedLogs ======
void FlashLogger::printFormattedLogs() {
  Serial.println("----------------------------------");

  // We’ll print grouped by day. Make a simple pass: for all sectors present, in order.
  int lastPrintedDay = -1;
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (!_index[s].present) continue;

    uint16_t day = _index[s].dayID;
    if ((int)day != lastPrintedDay) {
      // Print header for this (new) day
      // Convert dayID back to an approximate date: take now and subtract delta days
      DateTime now = _rtc->now();
      int32_t delta = (int32_t)dayIDFromDateTime(now) - (int32_t)day;
      DateTime dt = now - TimeSpan(delta * 86400L);

      char dateBuf[20];
      formatDate(dt, dateBuf, sizeof(dateBuf));
      Serial.printf("date %s\n{\n", dateBuf);
      lastPrintedDay = (int)day;
    }

    // Print sector content
    printSectorData(s);

    // Check if next sector belongs to same day; if not, close brace
    bool nextSameDay = false;
    for (int t = s + 1; t < MAX_SECTORS; ++t) {
      if (_index[t].present && _index[t].dayID == (uint16_t)lastPrintedDay) {
        nextSameDay = true;
        break;
      }
    }
    if (!nextSameDay) {
      Serial.println("}\n----------------------------------");
    }
  }
}

// ====== readAll (raw) ======
void FlashLogger::readAll() {
  Serial.println("=== RAW DUMP (all used sectors) ===");
  uint8_t buf[PAGE_SIZE];

  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (!_index[s].present) continue;
    uint32_t base = sectorBaseAddr(s);

    Serial.printf("\n[SECTOR %d @ 0x%06lX] dayID=%u pushed=%d\n",
                  s, base, _index[s].dayID, (int)_index[s].pushed);

    for (uint32_t addr = base + sizeof(SectorHeader);
         addr < base + SECTOR_SIZE;
         addr += PAGE_SIZE) {
      readData(addr, buf, PAGE_SIZE);
      for (int i = 0; i < PAGE_SIZE; ++i) {
        if (buf[i] != 0xFF && buf[i] != 0x00) Serial.write(buf[i]);
      }
    }
    Serial.println();
  }
  Serial.println("\n=== END RAW DUMP ===");
}

// ====== mark pushed ======
void FlashLogger::markDayPushed(uint16_t dayID) {
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (_index[s].present && _index[s].dayID == dayID) {
      _index[s].pushed = true;

      // Update header on flash
      SectorHeader hdr;
      readSectorHeader(s, hdr);
      hdr.pushed = 1;
      // Rewrite header (program over header bytes)
      writeEnable();
      uint32_t base = sectorBaseAddr(s);
      digitalWrite(_cs, LOW);
      SPI.transfer(CMD_PP);
      SPI.transfer((base >> 16) & 0xFF);
      SPI.transfer((base >> 8)  & 0xFF);
      SPI.transfer(base         & 0xFF);
      const uint8_t* p = (const uint8_t*)&hdr;
      for (uint16_t i = 0; i < sizeof(SectorHeader); ++i) SPI.transfer(p[i]);
      digitalWrite(_cs, HIGH);
      delay(3);
    }
  }
  Serial.printf("Marked day %u as pushed.\n", dayID);
}

void FlashLogger::markCurrentDayPushed() {
  markDayPushed(_currentDay);
}

// ====== gc ======
void FlashLogger::gc() {
  DateTime now = _rtc->now();
  uint16_t todayID = dayIDFromDateTime(now);

  Serial.println("🧹 GC: checking sectors...");
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (!_index[s].present) continue;
    if (!_index[s].pushed) continue;

    uint16_t d = _index[s].dayID;
    if (isOlderThanNDays(todayID, d, 7)) {
      uint32_t base = sectorBaseAddr(s);
      sectorErase(base);
      _index[s] = {false, 0, false, 0};
      Serial.printf("  erased sector %d (day=%u, >7 days & pushed)\n", s, d);
    }
  }
}

// ====== setDateStyle ======
void FlashLogger::setDateStyle(uint8_t style) {
  if (style < 1 || style > 3) return;
  _dateStyle = (DateStyle)style;
}

// ====== low-level flash ======
void FlashLogger::writeEnable() {
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_WREN);
  digitalWrite(_cs, HIGH);
}

void FlashLogger::readData(uint32_t addr, uint8_t* buf, uint16_t len) {
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_READ);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer(addr         & 0xFF);
  for (uint16_t i = 0; i < len; ++i) buf[i] = SPI.transfer(0x00);
  digitalWrite(_cs, HIGH);
}

void FlashLogger::pageProgram(uint32_t addr, const uint8_t* buf, uint16_t len) {
  writeEnable();
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_PP);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer(addr         & 0xFF);
  for (uint16_t i = 0; i < len; ++i) SPI.transfer(buf[i]);
  digitalWrite(_cs, HIGH);
  delay(5);
}

void FlashLogger::sectorErase(uint32_t addr) {
  writeEnable();
  digitalWrite(_cs, LOW);
  SPI.transfer(CMD_SE);
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer(addr         & 0xFF);
  digitalWrite(_cs, HIGH);
  delay(60);
}

// ====== sector/header helpers ======
bool FlashLogger::readSectorHeader(int sector, SectorHeader& hdr) {
  uint32_t base = sectorBaseAddr(sector);
  uint8_t buf[sizeof(SectorHeader)];
  readData(base, buf, sizeof(SectorHeader));
  memcpy(&hdr, buf, sizeof(SectorHeader));
  return (hdr.magic == 0x4C4F4747UL); // 'LOGG'
}

void FlashLogger::writeSectorHeader(int sector, uint16_t dayID, bool pushed) {
  SectorHeader hdr {};
  hdr.magic  = 0x4C4F4747UL; // 'LOGG'
  hdr.dayID  = dayID;
  hdr.pushed = pushed ? 1 : 0;
  hdr.reserved = 0;

  pageProgram(sectorBaseAddr(sector), (const uint8_t*)&hdr, sizeof(SectorHeader));
}

bool FlashLogger::sectorIsEmpty(int sector) {
  // A fresh sector should be all 0xFF; read header first byte
  uint8_t b;
  readData(sectorBaseAddr(sector), &b, 1);
  return (b == 0xFF);
}

void FlashLogger::scanAllSectorsBuildIndex() {
  for (int s = 0; s < MAX_SECTORS; ++s) {
    SectorHeader hdr;
    if (readSectorHeader(s, hdr)) {
      _index[s].present = true;
      _index[s].dayID   = hdr.dayID;
      _index[s].pushed  = (hdr.pushed != 0);
      // set a provisional writePtr (will refine later when/if selected)
      _index[s].writePtr = sectorBaseAddr(s) + sizeof(SectorHeader);
    }
  }
}

void FlashLogger::selectOrCreateTodaySector() {
  // Try to find last sector used for today (highest sector index with dayID == today)
  int lastForToday = -1;
  for (int s = MAX_SECTORS - 1; s >= 0; --s) {
    if (_index[s].present && _index[s].dayID == _currentDay) {
      lastForToday = s;
      break;
    }
  }
  if (lastForToday >= 0) {
    _currentSector = lastForToday;
    return;
  }

  // Otherwise, find first truly empty sector and initialize it for today
  for (int s = 0; s < MAX_SECTORS; ++s) {
    if (sectorIsEmpty(s)) {
      // Erase to be safe (and to get clean 0xFFs), then write header
      sectorErase(sectorBaseAddr(s));
      writeSectorHeader(s, _currentDay, false);
      _index[s] = {true, _currentDay, false, sectorBaseAddr(s) + sizeof(SectorHeader)};
      _currentSector = s;
      return;
    }
  }

  // No empty sectors -> out of space
  _currentSector = -1;
}

void FlashLogger::findLastWritePositionInSector(int sector) {
  uint32_t base = sectorBaseAddr(sector);
  uint32_t addr = base + sizeof(SectorHeader);
  uint8_t buf[PAGE_SIZE];

  for (; addr < base + SECTOR_SIZE; addr += PAGE_SIZE) {
    readData(addr, buf, PAGE_SIZE);
    if (buf[0] == 0xFF || buf[0] == 0x00) {
      _index[sector].writePtr = addr;
      return;
    }
  }
  // full
  _index[sector].writePtr = base + SECTOR_SIZE;
}

bool FlashLogger::sectorHasSpace(int sector, uint16_t needBytes) {
  uint32_t base = sectorBaseAddr(sector);
  uint32_t wp   = _index[sector].writePtr;
  return (wp + needBytes) <= (base + SECTOR_SIZE);
}

bool FlashLogger::moveToNextSectorSameDay() {
  // Find next empty sector
  for (int s = _currentSector + 1; s < MAX_SECTORS; ++s) {
    if (sectorIsEmpty(s)) {
      // Prepare for same day
      sectorErase(sectorBaseAddr(s));
      writeSectorHeader(s, _currentDay, false);
      _index[s] = {true, _currentDay, false, sectorBaseAddr(s) + sizeof(SectorHeader)};
      _currentSector = s;
      _writeAddr = _index[s].writePtr;
      Serial.printf("Rolled to next sector %d for day %u\n", s, _currentDay);
      return true;
    }
  }
  return false; // out of sectors
}

// ====== time helpers ======
uint16_t FlashLogger::dayIDFromDateTime(const DateTime& t) {
  // RTClib: seconds since 2000-01-01
  uint32_t sec2000 = t.secondstime();
  return (uint16_t)(sec2000 / 86400UL);
}

bool FlashLogger::isOlderThanNDays(uint16_t baseDay, uint16_t targetDay, uint16_t n) {
  // true if baseDay - targetDay >= n (and not underflow)
  if (baseDay >= targetDay) {
    return (baseDay - targetDay) >= n;
  }
  return false;
}

// ====== printing helpers ======
void FlashLogger::formatDate(const DateTime& dt, char* out, size_t outLen) const {
  switch (_dateStyle) {
    case DATE_THAI: snprintf(out, outLen, "%02d/%02d/%02d",
                             dt.day(), dt.month(), (dt.year() - 2000)); break;
    case DATE_ISO:  snprintf(out, outLen, "%04d-%02d-%02d",
                             dt.year(), dt.month(), dt.day()); break;
    case DATE_US:   snprintf(out, outLen, "%02d/%02d/%02d",
                             dt.month(), dt.day(), (dt.year() - 2000)); break;
    default:        snprintf(out, outLen, "%02d/%02d/%02d",
                             dt.day(), dt.month(), (dt.year() - 2000)); break;
  }
}

void FlashLogger::printSectorData(int sector) {
  uint8_t buf[PAGE_SIZE];
  uint32_t base = sectorBaseAddr(sector);

  for (uint32_t addr = base + sizeof(SectorHeader);
       addr < base + SECTOR_SIZE;
       addr += PAGE_SIZE) {
    readData(addr, buf, PAGE_SIZE);
    for (int i = 0; i < PAGE_SIZE; ++i) {
      if (buf[i] != 0xFF && buf[i] != 0x00) Serial.write(buf[i]);
    }
  }
}
