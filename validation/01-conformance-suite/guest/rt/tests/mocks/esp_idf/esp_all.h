#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define IRAM_ATTR
/* freertos */
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void vTaskDelay(TickType_t t);
#define taskYIELD() do {} while (0)
/* gpio */
typedef int gpio_num_t;
#define GPIO_IS_VALID_GPIO(g) ((g) >= 0 && (g) < 40)
#define GPIO_IS_VALID_OUTPUT_GPIO(g) ((g) >= 0 && (g) < 34)
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_INPUT_OUTPUT, GPIO_MODE_INPUT_OUTPUT_OD } gpio_mode_t;
typedef enum { GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY, GPIO_FLOATING } gpio_pull_mode_t;
typedef enum { GPIO_INTR_POSEDGE = 1, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE } gpio_int_type_t;
#define GPIO_PULLUP_ENABLE 1
esp_err_t gpio_reset_pin(gpio_num_t);
esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t);
esp_err_t gpio_set_pull_mode(gpio_num_t, gpio_pull_mode_t);
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
int gpio_get_level(gpio_num_t);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_set_intr_type(gpio_num_t, gpio_int_type_t);
esp_err_t gpio_isr_handler_add(gpio_num_t, void (*fn)(void *), void *arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_intr_enable(gpio_num_t);
/* ledc */
typedef enum { LEDC_LOW_SPEED_MODE } ledc_mode_t;
typedef enum { LEDC_TIMER_10_BIT = 10 } ledc_timer_bit_t;
typedef enum { LEDC_TIMER_0 } ledc_timer_t;
typedef enum { LEDC_AUTO_CLK } ledc_clk_cfg_t;
typedef int ledc_channel_t;
typedef struct { ledc_mode_t speed_mode; ledc_timer_bit_t duty_resolution; ledc_timer_t timer_num; uint32_t freq_hz; ledc_clk_cfg_t clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num; ledc_mode_t speed_mode; ledc_channel_t channel; ledc_timer_t timer_sel; uint32_t duty; int hpoint; } ledc_channel_config_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t *);
esp_err_t ledc_channel_config(const ledc_channel_config_t *);
esp_err_t ledc_set_freq(ledc_mode_t, ledc_timer_t, uint32_t);
esp_err_t ledc_set_duty(ledc_mode_t, ledc_channel_t, uint32_t);
esp_err_t ledc_update_duty(ledc_mode_t, ledc_channel_t);
/* uart */
typedef int uart_port_t;
#define UART_NUM_MAX 3
#define UART_PIN_NO_CHANGE (-1)
typedef enum { UART_DATA_8_BITS = 3 } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE } uart_hw_flowcontrol_t;
typedef struct { int baud_rate; uart_word_length_t data_bits; uart_parity_t parity; uart_stop_bits_t stop_bits; uart_hw_flowcontrol_t flow_ctrl; } uart_config_t;
esp_err_t uart_driver_install(uart_port_t, int rx, int tx, int qsize, void *queue, int flags);
esp_err_t uart_param_config(uart_port_t, const uart_config_t *);
esp_err_t uart_set_pin(uart_port_t, int tx, int rx, int rts, int cts);
int uart_write_bytes(uart_port_t, const char *, size_t);
int uart_read_bytes(uart_port_t, void *, uint32_t, TickType_t);
esp_err_t uart_driver_delete(uart_port_t);
/* i2c (legacy driver) */
typedef int i2c_port_t;
#define I2C_NUM_MAX 2
typedef enum { I2C_MODE_MASTER = 1 } i2c_mode_t;
typedef struct { i2c_mode_t mode; int sda_io_num; int scl_io_num; int sda_pullup_en; int scl_pullup_en; struct { uint32_t clk_speed; } master; } i2c_config_t;
esp_err_t i2c_param_config(i2c_port_t, const i2c_config_t *);
esp_err_t i2c_driver_install(i2c_port_t, i2c_mode_t, size_t, size_t, int);
esp_err_t i2c_master_write_to_device(i2c_port_t, uint8_t, const uint8_t *, size_t, TickType_t);
esp_err_t i2c_master_read_from_device(i2c_port_t, uint8_t, uint8_t *, size_t, TickType_t);
esp_err_t i2c_master_write_read_device(i2c_port_t, uint8_t, const uint8_t *, size_t, uint8_t *, size_t, TickType_t);
/* spi */
typedef enum { SPI2_HOST = 1, SPI3_HOST } spi_host_device_t;
#define SPI_DMA_CH_AUTO 3
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num; int max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz; uint8_t mode; int spics_io_num; int queue_size; } spi_device_interface_config_t;
typedef struct spi_device_t *spi_device_handle_t;
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t *, int dma);
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t *);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_free(spi_host_device_t);
/* system */
int64_t esp_timer_get_time(void);
void esp_restart(void);
void esp_deep_sleep(uint64_t);
uint32_t esp_random(void);
void esp_rom_delay_us(uint32_t);
esp_err_t esp_task_wdt_reset(void);
#define ESP_IDF_VERSION_VAL(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 3, 0)
/* adc oneshot (IDF >= 5) */
typedef struct adc_oneshot_unit_ctx_t *adc_oneshot_unit_handle_t;
typedef enum { ADC_UNIT_1, ADC_UNIT_2 } adc_unit_t;
typedef int adc_channel_t;
typedef struct { adc_unit_t unit_id; int ulp_mode; } adc_oneshot_unit_init_cfg_t;
typedef struct { int atten; int bitwidth; } adc_oneshot_chan_cfg_t;
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_DEFAULT 0
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t *);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int *);
esp_err_t adc_oneshot_io_to_channel(int, adc_unit_t *, adc_channel_t *);
