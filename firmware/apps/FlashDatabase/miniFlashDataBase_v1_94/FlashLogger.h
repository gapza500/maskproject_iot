#ifndef FLASH_LOGGER_H
#define FLASH_LOGGER_H

#include <Arduino.h>
#include <SPI.h>
#include <RTClib.h>

// =========================
// Flash geometry & commands
// =========================
#define PAGE_SIZE     256
#define SECTOR_SIZE   4096
#define MAX_SECTORS   2048   // 8MB / 4KB

// Winbond W25Qxx
#define CMD_READ      0x03
#define CMD_PP        0x02
#define CMD_WREN      0x06
#define CMD_SE        0x20

// =========================
// On-flash structures
// =========================

// ---- Sector header (v1.8 adds generation) ----
struct SectorHeader {
  uint32_t magic;       // 'LOGG' = 0x4C4F4747
  uint16_t dayID;       // days since 2000-01-01
  uint8_t  pushed;      // 0 = not pushed, 1 = pushed
  uint8_t  reserved;    // padding
  uint32_t generation;  // boot/generation id when this sector started
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
  uint32_t magic;            // 'FACT' = 0x46414354
  char     model[24];        // e.g. "AirMonitor C6"
  char     flashModel[16];   // e.g. "W25Q64JV"
  char     deviceId[16];     // 12 digits + '\0'
  uint16_t firstDayID;       // dayID when first set
  uint8_t  defaultDateStyle; // 1=TH,2=ISO,3=US
  uint8_t  reserved0;
  uint32_t totalEraseOps;

  // v1.8 wear-leveling & quarantine
  uint32_t bootCounter;      // increments each begin()
  uint16_t startHint;        // round-robin day start sector hint
  uint16_t badCount;         // number of quarantined sectors
  uint16_t badList[16];      // up to 16 bad sectors

  uint8_t  reserved[28];     // future use
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

// ---- Date style ----
enum DateStyle : uint8_t {
  DATE_THAI = 1,   // DD/MM/YY
  DATE_ISO  = 2,   // YYYY-MM-DD
  DATE_US   = 3    // MM/DD/YY
};

// ---- v1.7+ record header + commit marker ----
struct __attribute__((packed)) RecordHeader {
  uint16_t len;   // payload length (no header/commit)
  uint16_t crc;   // CRC16 over payload
  uint32_t ts;    // seconds since 2000-01-01 (RTClib secondstime)
  uint32_t seq;   // monotonic sequence
  uint8_t  flags; // reserved
  uint8_t  rsv;   // reserved
};
static constexpr uint8_t REC_COMMIT = 0xA5;

// =========================
// v1.91 summaries & shell
// =========================
struct DaySummary {
  uint16_t dayID;
  uint16_t sectors;
  uint32_t bytes;
  bool     pushed;
  uint32_t firstTs;
  uint32_t lastTs;
};

struct SectorSummary {
  int      sector;
  uint16_t dayID;
  uint32_t bytes;
  bool     pushed;
  uint32_t firstTs;
  uint32_t lastTs;
};

enum SelKind { SEL_NONE, SEL_DAY, SEL_SECTOR };

// =========================
// v1.92 query engine
// =========================
enum OutFmt { OUT_JSONL = 0, OUT_CSV = 1 };

struct QuerySpec {
  // time filters
  uint32_t ts_from = 0;            // inclusive (seconds since 2000-01-01)
  uint32_t ts_to   = 0xFFFFFFFF;   // inclusive
  uint16_t day_from = 0;           // if set, overrides ts_*
  uint16_t day_to   = 0;

  // field filter (OR of keys; nullptr-terminated)
  const char* includeKeys[8] = { nullptr }; // e.g. {"bat","temp",nullptr}

  // limits / sampling
  uint32_t max_records = 0;        // 0 = no limit
  uint16_t sample_every = 1;       // 1 = every record

  // output
  OutFmt out = OUT_JSONL;
  bool compact_json = true;        // for JSONL (ignored for CSV)
};

typedef void (*RowCallback)(const char* line, void* user);

// =========================
// v1.93 sync cursors
// =========================
struct SyncCursor {
  uint16_t dayID;     // day of next record to read
  int      sector;    // absolute sector index of next record
  uint32_t addr;      // absolute flash address of next record (RecordHeader addr)
  uint32_t seq_next;  // next expected writer seq (informational)
};

// =========================
// v1.94 parameterized config
// =========================
struct FlashLoggerConfig {
  // Hardware
  int      spi_cs_pin       = 4;
  int      spi_sck_pin      = -1;    // -1 = default VSPI/HSPI
  int      spi_mosi_pin     = -1;
  int      spi_miso_pin     = -1;
  uint32_t spi_clock_hz     = 20'000'000;

  // Time / RTC
  RTC_DS3231* rtc           = nullptr; // required
  DateStyle   dateStyle     = DATE_THAI;

  // Flash layout / policy
  uint32_t totalSizeBytes   = 8 * 1024 * 1024; // 8MB default
  uint32_t sectorSize       = SECTOR_SIZE;     // keep 4KB sectors
  uint16_t retentionDays    = 7;               // GC erase after pushed+N days
  uint32_t dailyBytesHint   = 3500;            // for stats/estimate
  uint16_t maxSectorsPerDay = 64;              // safety cap

  // Factory & identity (set once if empty)
  const char* model         = "AirMonitor C6";
  const char* flashModel    = "W25Q64JV";
  const char* deviceId      = "001245678912";
  const char* resetCode12   = "847291506314";  // 12-digit confirm

  // Output defaults
  OutFmt     defaultOut     = OUT_JSONL;
  const char* csvColumns    = "ts,bat,temp";

  // Shell
  bool enableShell          = true;  // enable built-in commands
};

// ===== v1.95: Persisted cursor & anchors & diagnostics =====
struct Anchor {
  int      sector;
  uint16_t dayID;
  uint32_t firstTs;
  uint32_t firstAddr;   // absolute addr of first valid record
  uint32_t lastTs;
};

static constexpr int MAX_ANCHORS = 128;

// =========================
// FlashLogger class
// =========================
class FlashLogger {
public:
  // --- constructors ---
  FlashLogger() = default;                 // v1.94 preferred (use begin(config))
  explicit FlashLogger(uint8_t csPin);     // legacy ctor (kept for compat)

  // --- initialization ---
  bool begin(const FlashLoggerConfig& cfg); // v1.94 parameterized init
  bool begin(RTC_DS3231* rtc);              // legacy init (uses ctor CS pin)

  // --- writing ---
  bool append(const String& json);          // crash-safe append (adds '\n')

  // --- printing / debug ---
  void printFormattedLogs();                // grouped by day, date style respected
  void readAll();                           // raw valid records (debug)

  // --- push status & GC ---
  void markDayPushed(uint16_t dayID);
  void markCurrentDayPushed();
  void markDaysPushedUntil(uint16_t dayID_inclusive);   // v1.93
  void gc();                                            // erase pushed & old

  // --- presentation / formatting ---
  void setDateStyle(uint8_t style);
  void setOutputFormat(OutFmt f);
  void setCsvColumns(const char* cols_csv);
  const char* getCsvColumns() const { return _csvCols; }

  // --- factory info / reset ---
  bool setFactoryInfo(const String& model, const String& flashModel, const String& deviceID);
  void printFactoryInfo();
  bool factoryReset(const String& code12); // keep factory info, wipe logs

  // --- stats / health ---
  FlashStats getFlashStats(float avgBytesPerDay);
  float getFreeSpaceMB();
  float getUsedSpaceMB();
  float getUsedPercent();
  float getFlashHealth(); // from totalEraseOps
  uint16_t estimateDaysRemaining(float avgBytesPerDay);

  // --- back-pressure (optional daily cap) ---
  void setMaxDailyBytes(uint32_t bytes); // 0 = unlimited
  bool isLowSpace() const;

  // --- v1.91 navigation/shell ---
  void    buildSummaries();
  void    listDays();                      // "ls"
  void    listSectors(uint16_t dayID);     // "ls sectors [dayID]"
  bool    selectDay(uint16_t dayID);       // "cd day …"
  bool    selectSector(int sector);        // "cd sector …"
  void    printSelected();                 // "print"
  void    printSelectedInfo();             // "info"

  // --- serial shell entrypoint ---
  bool    handleCommand(const String& cmd, Stream& io);

  // --- date helpers for UI/INO ---
  static  bool parseDateYYYYMMDD(const String& s, uint16_t& outDayID);
  void    formatDayID(uint16_t dayID, char* out, size_t outLen) const;
  DateTime nowRTC() const;                                 // returns _rtc->now()
  uint16_t dayIDFromDateTime(const DateTime& dt) const;    // public wrapper

  // --- expose last caches for UI ---
  int     lastDayCount() const { return _dayCount; }
  int     lastSectorCount() const { return _sectCount; }
  bool    dayIndexToDayID(int idx, uint16_t& out) const;   // 0-based
  bool    sectorIndexToSector(int idx, int& out) const;    // 0-based

  // --- v1.92 query API ---
  uint32_t queryLogs(const QuerySpec& q, RowCallback onRow, void* user);
  uint32_t queryLatest(uint32_t N, RowCallback onRow, void* user);
  uint32_t queryRange(uint32_t ts_from, uint32_t ts_to, RowCallback onRow, void* user);
  uint32_t queryBattery(RowCallback onRow, void* user);
  bool     handleQueryCommand(const String& cmd, Stream& io);

  // --- v1.93 cursor/export API ---
  bool     getCursor(SyncCursor& out) const;
  bool     setCursor(const SyncCursor& in);
  void     clearCursor();
  uint32_t exportSince(const SyncCursor& from, uint32_t max_rows, RowCallback onRow, void* user);
  bool     handleCursorCommand(const String& cmd, Stream& io);

  // Persist cursor in ESP32 NVS (Preferences)
  bool saveCursorNVS(const char* ns="flog", const char* key="cursor");
  bool loadCursorNVS(SyncCursor& out, const char* ns="flog", const char* key="cursor");
  bool loadCursorNVS(const char* ns="flog", const char* key="cursor") {
    SyncCursor c{}; if (!loadCursorNVS(c, ns, key)) return false; return setCursor(c);
  }

  // Diagnostics
  void scanBadAndQuarantine(Stream& io);   // "scanbad" shell command

  // Shell additions (already routed via handleCommand):
  //   cursor save [ns key]
  //   cursor load [ns key]
  //   scanbad

private:
  // ===== config & runtime =====
  FlashLoggerConfig _cfg{};          // v1.94 stored config
  OutFmt  _outFmt = OUT_JSONL;
  char    _csvCols[48] = "ts,bat";
  RTC_DS3231* _rtc = nullptr;

  // Legacy ctor pin (if used)
  uint8_t     _cs = 4;

  // time & write head
  uint16_t    _currentDay   = 0;
  int         _currentSector = -1;
  uint32_t    _writeAddr     = 0;
  DateStyle   _dateStyle     = DATE_THAI;
  uint32_t    _seqCounter    = 0;
  uint32_t    _generation    = 0;

  // daily cap
  uint32_t    _maxDailyBytes = 0;
  uint32_t    _todayBytes     = 0;
  bool        _lowSpace       = false;

  // per-sector RAM index
  SectorIndex _index[MAX_SECTORS];

  // factory storage (last sector)
  FactoryInfo _factory {};
  static constexpr int FACTORY_SECTOR = MAX_SECTORS - 1;

  // ===== low level flash =====
  void writeEnable();
  void readData(uint32_t addr, uint8_t* buf, uint16_t len);
  void pageProgram(uint32_t addr, const uint8_t* buf, uint32_t len); // chunk-safe
  void sectorErase(uint32_t addr, bool countErase = true);

  // ===== sector/header helpers =====
  static uint32_t sectorBaseAddr(int sector) { return (uint32_t)sector * SECTOR_SIZE; }
  bool   readSectorHeader(int sector, SectorHeader& hdr);
  void   writeSectorHeader(int sector, uint16_t dayID, bool pushed);
  bool   sectorIsEmpty(int sector);
  void   scanAllSectorsBuildIndex();
  void   selectOrCreateTodaySector();
  void   findLastWritePositionInSector(int sector); // header-aware
  bool   sectorHasSpace(int sector, uint32_t needBytes);
  bool   moveToNextSectorSameDay();

  // wear-leveling & quarantine
  bool   isBadSector(int sector) const;
  void   quarantineSector(int sector);
  int    nextRoundRobinStart();

  // verify ops
  bool   verifyErase(uint32_t base);
  bool   verifyWrite(uint32_t addr, const uint8_t* buf, uint32_t len);

  // factory info helpers
  bool   loadFactoryInfo();
  void   saveFactoryInfo();
  static uint16_t dayIDFromDateTime_static(const DateTime& t); // internal static

  // time helpers
  static bool isOlderThanNDays(uint16_t baseDay, uint16_t targetDay, uint16_t n);

  // printing helpers
  void formatDate(const DateTime& dt, char* out, size_t outLen) const;
  void printSectorData(int sector);

  // capacity helpers
  uint32_t countUsedSectors() const;

  // CRC16 helper
  static uint16_t crc16(const uint8_t* data, uint32_t len, uint16_t seed = 0xFFFF);

  // ===== navigation caches =====
  static constexpr int MAX_DAYS_CACHE  = 366;
  static constexpr int MAX_SECT_CACHE  = 64;

  DaySummary    _days[MAX_DAYS_CACHE];
  int           _dayCount = 0;

  SectorSummary _sects[MAX_SECT_CACHE];
  int           _sectCount = 0;

  // selection
  SelKind       _selKind = SEL_NONE;
  uint16_t      _selDay  = 0;
  int           _selSector = -1;

  // summarize & sort
  void      summarizeDay(uint16_t dayID, DaySummary& out);
  void      summarizeSector(int sector, SectorSummary& out);
  uint32_t  computeValidBytesInSector(int sector, uint32_t& firstTs, uint32_t& lastTs);
  static    int cmpDayDesc(const void* a, const void* b);

  // ===== v1.92 helpers =====
  static bool recordMatchesTime(uint16_t recDay, uint32_t ts, const QuerySpec& q);
  bool   emitRecord(uint16_t recDay, uint32_t ts, const uint8_t* payload, uint16_t len,
                    const QuerySpec& q, RowCallback onRow, void* user);
  bool   jsonExtractKeyValue(const char* json, const char* key, String& outVal) const;
  void   buildCsvLine(uint32_t ts, const char* payload, uint16_t len, const char* cols, String& out);
  void   buildJsonFiltered(const char* payload, uint16_t len, const char* const* keys, bool compact, String& out);

  // ===== v1.93 cursor state & helpers =====
  SyncCursor _readCursor{0, -1, 0, 0};

  bool    findFirstRecord(int sector, uint32_t& outAddr);
  bool    findNextRecordAddr(int sector, uint32_t curAddr, uint32_t& nextAddr);
  bool    readRecordMeta(uint32_t addr, RecordHeader& rh, uint16_t& recDay) const;
  bool    isValidRecordAt(uint32_t addr) const;
  bool    advanceToNextValid(SyncCursor& c) const;
  bool    earliestCursor(SyncCursor& out) const;

  // reinit after factory reset
  void    reinitAfterFactoryReset();

  // Anchor index for faster range scans
  Anchor  _anchors[MAX_ANCHORS];
  int     _anchorCount = 0;
  void    buildAnchors();                  // build from current index
  // fast sector prefilter for day/ts range
  bool    sectorMaybeInRangeByAnchor(int sector, uint16_t dayFrom, uint16_t dayTo,
                                     uint32_t ts_from, uint32_t ts_to) const;

  // small helpers
  bool    firstRecordInSector(int sector, uint32_t& firstAddr, uint32_t& firstTs, uint32_t& lastTs);
};

// =========================
// Inline small public utils
// =========================
inline DateTime FlashLogger::nowRTC() const { return _rtc ? _rtc->now() : DateTime(2000,1,1,0,0,0); }
inline uint16_t FlashLogger::dayIDFromDateTime(const DateTime& dt) const {
  return (uint16_t)(dt.secondstime() / 86400UL);
}

#endif // FLASH_LOGGER_H
