#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef unsigned int uint;
#define NUM_BANK0_GPIOS 30
enum gpio_function { GPIO_FUNC_SPI = 1, GPIO_FUNC_UART = 2, GPIO_FUNC_I2C = 3, GPIO_FUNC_PWM = 4 };
#define GPIO_IN 0
#define GPIO_OUT 1
#define GPIO_IRQ_EDGE_FALL 4u
#define GPIO_IRQ_EDGE_RISE 8u
void gpio_init(uint); void gpio_set_dir(uint, bool); void gpio_disable_pulls(uint); void gpio_pull_up(uint); void gpio_pull_down(uint);
void gpio_put(uint, bool); bool gpio_get(uint); void gpio_set_function(uint, enum gpio_function);
typedef void (*gpio_irq_callback_t)(uint, uint32_t);
void gpio_set_irq_enabled_with_callback(uint, uint32_t, bool, gpio_irq_callback_t);
void gpio_set_irq_enabled(uint, uint32_t, bool);
enum clock_index { clk_sys = 5 }; uint32_t clock_get_hz(enum clock_index);
uint pwm_gpio_to_slice_num(uint); void pwm_set_clkdiv(uint, float); void pwm_set_wrap(uint, uint16_t);
void pwm_set_gpio_level(uint, uint16_t); void pwm_set_enabled(uint, bool);
void adc_init(void); void adc_gpio_init(uint); void adc_select_input(uint); uint16_t adc_read(void);
uint64_t time_us_64(void); uint32_t time_us_32(void); void sleep_ms(uint32_t); void sleep_us(uint64_t); void tight_loop_contents(void);
void watchdog_reboot(uint32_t, uint32_t, uint32_t); void watchdog_update(void); uint32_t get_rand_32(void);
typedef struct uart_inst uart_inst_t; extern uart_inst_t *uart0, *uart1;
uint uart_init(uart_inst_t *, uint); void uart_deinit(uart_inst_t *); void uart_write_blocking(uart_inst_t *, const uint8_t *, size_t);
bool uart_is_readable_within_us(uart_inst_t *, uint32_t); char uart_getc(uart_inst_t *);
typedef struct i2c_inst i2c_inst_t; extern i2c_inst_t *i2c0, *i2c1;
uint i2c_init(i2c_inst_t *, uint); int i2c_write_blocking(i2c_inst_t *, uint8_t, const uint8_t *, size_t, bool);
int i2c_read_blocking(i2c_inst_t *, uint8_t, uint8_t *, size_t, bool);
typedef struct spi_inst spi_inst_t; extern spi_inst_t *spi0, *spi1;
typedef enum { SPI_CPOL_0, SPI_CPOL_1 } spi_cpol_t; typedef enum { SPI_CPHA_0, SPI_CPHA_1 } spi_cpha_t; typedef enum { SPI_LSB_FIRST, SPI_MSB_FIRST } spi_order_t;
uint spi_init(spi_inst_t *, uint); void spi_deinit(spi_inst_t *); void spi_set_format(spi_inst_t *, uint, spi_cpol_t, spi_cpha_t, spi_order_t);
int spi_write_read_blocking(spi_inst_t *, const uint8_t *, uint8_t *, size_t); int spi_write_blocking(spi_inst_t *, const uint8_t *, size_t);
