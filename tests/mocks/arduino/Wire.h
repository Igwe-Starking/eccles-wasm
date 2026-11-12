#pragma once
#include <Arduino.h>
class TwoWire { public: void begin(); void setClock(uint32_t); void beginTransmission(uint8_t); size_t write(const uint8_t *, size_t);
  uint8_t endTransmission(bool stop = true); uint8_t requestFrom(uint8_t addr, uint8_t n); int read(); };
extern TwoWire Wire;
