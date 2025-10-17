#ifndef FLASH_LOGGER_H
#define FLASH_LOGGER_H

#include <Arduino.h>
#include <SPI.h>
#include <RTClib.h>

// ---- Flash geometry ----
#define PAGE_SIZE     256
#define SECTOR_SIZE   4096
#define MAX_SECTORS   2048   // 8MB / 4KB

// ---- SPI commands (Winbond W25Qxx) ----
#define CMD_READ      0x03
#define CMD_PP        0x02
#define CMD_WREN      0x06
#define CMD_SE        0x20

// ---- Sector header (persist per sector, after erase) ----
struct SectorHeader {
  uint32_t magic;     // 'LOGG' = 0x4C4F4747
  uint16_t dayID;     // days since 2000-01-01; 0xFFFF = free/unassigned
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

// ---- Factory info (binary, in last sector) ----
struct FactoryInfo {
  uint32_t magic;        // 'FACT' = 0x46414354
  char     model[24];    // default "AirMonitor C6"
  char     flashModel[16]; // e.g. "W25Q64JV"
  char     deviceId[16]; // 12 digits + '\0'
  uint16_t firstDayID;   // dayID when first set
  uint8_t  defaultDateStyle; // 1=TH,2=ISO,3=US
  uint8_t  reserved0;
  uint32_t totalEraseOps; // accumulate all erases across device lifetime
  uint8_t  reserved[64];  // future use
};

// ---- Flash stats for UI ----
struct FlashStats {
  float totalMB;
  float usedMB;
  float freeMB;
  float usedPercent;
  float healthPercent;
  uint16_t estimatedDaysLeft;
};

enum DateStyle : uint8_t {
  DATE_THAI = 1,   // DD/MM/YY
  DATE_ISO  = 2,   // YYYY-MM-DD
  DATE_US   = 3    // MM/DD/YY
};

// ---- v1.7 record header + commit marker ----
struct __attribute__((packed)) RecordHeader {
  uint16_t len;   // payload length (no header/commit)
  uint16_t crc;   // CRC16 over payload
  uint32_t ts;    // seconds since 2000-01-01 (RTClib secondstime)
  uint32_t seq;   // monotonic sequence
  uint8_t  flags; // reserved
  uint8_t  rsv;   // reserved
};
static constexpr uint8_t REC_COMMIT = 0xA5;

class FlashLogger {
public:
  explicit FlashLogger(uint8_t csPin);

  // Init (RTC must be rtc.begin()'d). Also loads/creates Factory Info.
  bool begin(RTC_DS3231* rtc);

  // Append JSON record (auto-add '\n', crash-safe, CRC'd)
  bool append(const String& json);

  // Pretty print grouped by day with selected date style (CRC/commit enforced)
  void printFormattedLogs();

  // Raw continuous dump (debug; still CRC/commit aware so it won't spew garbage)
  void readAll();

  // Mark a day (all its sectors) as "pushed"
  void markDayPushed(uint16_t dayID);
  void markCurrentDayPushed();

  // Safe GC: erase only when pushed && older than 7 days
  void gc();

  // Date style: 1=Thai, 2=ISO, 3=US
  void setDateStyle(uint8_t style);

  // ---- Factory info API ----
  bool setFactoryInfo(const String& model, const String& flashModel, const String& deviceID);
  void printFactoryInfo();
  bool factoryReset(const String& code12); // keep factory info, wipe everything else

  // ---- Stats / lifetime for UI ----
  FlashStats getFlashStats(float avgBytesPerDay);
  float getFreeSpaceMB();
  float getUsedSpaceMB();
  float getUsedPercent();
  float getFlashHealth(); // based on totalEraseOps / (MAX_SECTORS * 100000)
  uint16_t estimateDaysRemaining(float avgBytesPerDay);

private:
  // HW & state
  RTC_DS3231* _rtc = nullptr;
  uint8_t     _cs;
  uint16_t    _currentDay   = 0;
  int         _currentSector = -1;
  uint32_t    _writeAddr     = 0;
  DateStyle   _dateStyle     = DATE_THAI;
  uint32_t    _seqCounter    = 0; // v1.7 monotonic sequence

  // RAM index per sector
  SectorIndex _index[MAX_SECTORS];

  // Factory storage (last sector)
  FactoryInfo _factory {};
  static constexpr int FACTORY_SECTOR = MAX_SECTORS - 1;

  // Low-level flash
  void writeEnable();
  void readData(uint32_t addr, uint8_t* buf, uint16_t len);
  void pageProgram(uint32_t addr, const uint8_t* buf, uint32_t len); // v1.7 chunk-safe
  void sectorErase(uint32_t addr, bool countErase = true);

  // Sector/header helpers
  static uint32_t sectorBaseAddr(int sector) { return (uint32_t)sector * SECTOR_SIZE; }
  bool   readSectorHeader(int sector, SectorHeader& hdr);
  void   writeSectorHeader(int sector, uint16_t dayID, bool pushed);
  bool   sectorIsEmpty(int sector);
  void   scanAllSectorsBuildIndex();
  void   selectOrCreateTodaySector();

  // v1.7: header-aware scan to last valid record
  void   findLastWritePositionInSector(int sector);

  bool   sectorHasSpace(int sector, uint32_t needBytes);
  bool   moveToNextSectorSameDay();

  // Factory info helpers
  bool loadFactoryInfo();
  void saveFactoryInfo();
  static uint16_t dayIDFromDateTime(const DateTime& t);

  // Time helpers
  static bool isOlderThanNDays(uint16_t baseDay, uint16_t targetDay, uint16_t n);

  // Printing helpers (header-aware)
  void formatDate(const DateTime& dt, char* out, size_t outLen) const;
  void printSectorData(int sector); // reads header->payload->commit with CRC

  // Capacity helpers
  uint32_t countUsedSectors() const;

  // CRC16 helper
  static uint16_t crc16(const uint8_t* data, uint32_t len, uint16_t seed = 0xFFFF);
};

#endif
