#ifndef MOCK_ARDUINO_H
#define MOCK_ARDUINO_H
#include <stdint.h>
#include <stddef.h>
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define OUTPUT_OPEN_DRAIN 5
#ifdef __cplusplus
extern "C" {
#endif
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);
void analogWrite(uint8_t pin, int val);
unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield(void);
#ifdef __cplusplus
}
class Print { public: virtual size_t write(const uint8_t *b, size_t n) = 0; virtual ~Print() {} };
class Stream : public Print { public: void setTimeout(unsigned long); size_t readBytes(char *b, size_t n); };
#define MSBFIRST 1
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3
#endif
#endif
