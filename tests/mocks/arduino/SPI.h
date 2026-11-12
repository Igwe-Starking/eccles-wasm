#pragma once
#include <Arduino.h>
class SPISettings { public: SPISettings(uint32_t clk, uint8_t order, uint8_t mode); };
class SPIClass { public: void begin(); void end(); void beginTransaction(SPISettings); void endTransaction(); uint8_t transfer(uint8_t v); };
extern SPIClass SPI;
