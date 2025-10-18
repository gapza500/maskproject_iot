#ifndef FLASH_LOGGER_H
#define FLASH_LOGGER_H

#include <Arduino.h>
#include <SPI.h>
#include <RTClib.h>

// ---- Flash constants ----
#define PAGE_SIZE     256
#define SECTOR_SIZE   4096
#define MAX_SECTORS   2048   // 8MB / 4KB

// ---- SPI commands (Winbond W25Qxx) ----
#define CMD_READ      0x03
#define CMD_PP        0x02
#define CMD_WREN      0x06
#define CMD_SE        0x20

// ---- Sector header (8 bytes) ----
struct SectorHeader {
  uint32_t magic;     // 'LOGG' = 0x4C4F4747
  uint16_t dayID;     // days since 2000-01-01
  uint8_t  pushed;    // 0 = not pushed, 1 = pushed
  uint8_t  reserved;  // padding
};

// ---- RAM index for quick lookups ----
struct SectorIndex {
  bool     present;   // header found
  uint16_t dayID;
  bool     pushed;
  uint32_t writePtr;  // absolute next-write address inside this sector
};

enum DateStyle : uint8_t {
  DATE_THAI = 1,   // DD/MM/YY
  DATE_ISO  = 2,   // YYYY-MM-DD
  DATE_US   = 3    // MM/DD/YY
};

class FlashLogger {
public:
  explicit FlashLogger(uint8_t csPin);

  // Call once in setup. Provide RTC instance that is already rtc.begin()'d.
  bool begin(RTC_DS3231* rtc);

  // Append one JSON record (null-terminated is fine).
  bool append(const String& json);

  // Pretty print grouped by day, with separators & chosen date style
  void printFormattedLogs();

  // Raw dump of all used sectors (continuous bytes)
  void readAll();

  // Mark a day (all its sectors) as "pushed" to enable GC later
  void markDayPushed(uint16_t dayID);
  void markCurrentDayPushed();

  // Safe GC: erase sectors only if pushed && older than 7 days
  void gc();

  // Change date style at runtime: 1=Thai, 2=ISO, 3=US
  void setDateStyle(uint8_t style);

private:
  // ---- HW/State ----
  RTC_DS3231* _rtc = nullptr;
  uint8_t     _cs;
  uint16_t    _currentDay   = 0;      // days since 2000-01-01
  int         _currentSector = -1;    // sector index currently writing
  uint32_t    _writeAddr     = 0;     // absolute flash address to write next
  DateStyle   _dateStyle     = DATE_THAI;

  // One index entry per sector (RAM)
  SectorIndex _index[MAX_SECTORS];

  // ---- Low-level flash ----
  void writeEnable();
  void readData(uint32_t addr, uint8_t* buf, uint16_t len);
  void pageProgram(uint32_t addr, const uint8_t* buf, uint16_t len);
  void sectorErase(uint32_t addr);

  // ---- Sector/header helpers ----
  bool   readSectorHeader(int sector, SectorHeader& hdr);
  void   writeSectorHeader(int sector, uint16_t dayID, bool pushed);
  bool   sectorIsEmpty(int sector);              // no header (0xFFs)
  void   scanAllSectorsBuildIndex();             // populate _index from headers
  void   selectOrCreateTodaySector();            // choose sector to write today
  void   findLastWritePositionInSector(int sector);

  // ---- Rolling to next sector if full ----
  bool   sectorHasSpace(int sector, uint16_t needBytes);
  bool   moveToNextSectorSameDay();

  // ---- Time helpers ----
  static uint16_t dayIDFromDateTime(const DateTime& t); // days since 2000-01-01
  static bool     isOlderThanNDays(uint16_t baseDay, uint16_t targetDay, uint16_t n);

  // ---- Printing helpers ----
  void   formatDate(const DateTime& dt, char* out, size_t outLen) const;
  void   printSectorData(int sector);

  // ---- Utils ----
  static uint32_t sectorBaseAddr(int sector) { return (uint32_t)sector * SECTOR_SIZE; }
};

#endif
