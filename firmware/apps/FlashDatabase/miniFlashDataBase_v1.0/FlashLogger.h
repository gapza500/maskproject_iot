#ifndef FLASH_LOGGER_H
#define FLASH_LOGGER_H

#include <Arduino.h>
#include <SPI.h>

#define PAGE_SIZE     256
#define SECTOR_SIZE   4096
#define CMD_READ      0x03
#define CMD_PP        0x02
#define CMD_WREN      0x06
#define CMD_SE        0x20
#define MAX_SECTORS   2048   // 8MB / 4KB

struct SectorIndex {
  uint16_t dayID;
  uint16_t sectorID;
  bool used;
};

class FlashLogger {
public:
  FlashLogger(uint8_t csPin);
  bool begin();
  bool append(const String &json);
  void readAll();
  void listSectors();

private:
  uint8_t _cs;
  uint32_t _writeAddr = 0;
  uint16_t _currentDay = 0;
  uint16_t _currentSector = 0;
  SectorIndex indexTable[MAX_SECTORS];

  void writeEnable();
  void sectorErase(uint32_t addr);
  void pageProgram(uint32_t addr, const uint8_t *buf, uint16_t len);
  void readData(uint32_t addr, uint8_t *buf, uint16_t len);
  uint32_t getDayID();
  void findLastWritePosition();
  void moveToNextSector();
};

#endif
