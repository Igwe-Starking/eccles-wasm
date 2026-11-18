#pragma once
#include <stdint.h>
typedef struct { uint32_t dummy; } GPIO_TypeDef;
extern GPIO_TypeDef GPIOA_s, GPIOB_s, GPIOC_s;
#define GPIOA (&GPIOA_s)
#define GPIOB (&GPIOB_s)
#define GPIOC (&GPIOC_s)
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
#define GPIO_MODE_INPUT 0u
#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_MODE_OUTPUT_OD 0x11u
#define GPIO_NOPULL 0u
#define GPIO_PULLUP 1u
#define GPIO_PULLDOWN 2u
#define GPIO_SPEED_FREQ_LOW 0u
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *);
void HAL_GPIO_WritePin(GPIO_TypeDef *, uint16_t, GPIO_PinState);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *, uint16_t);
uint32_t HAL_GetTick(void); void HAL_Delay(uint32_t);
typedef struct { volatile uint32_t LOAD, VAL; } SysTick_Type; extern SysTick_Type *SysTick_p;
#define SysTick SysTick_p
void NVIC_SystemReset(void);
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
typedef enum { HAL_OK = 0, HAL_ERROR } HAL_StatusTypeDef;
typedef struct { int x; } UART_HandleTypeDef; typedef struct { int x; } I2C_HandleTypeDef; typedef struct { int x; } SPI_HandleTypeDef;
#define HAL_UART_MODULE_ENABLED
#define HAL_I2C_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *, uint16_t, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *, uint16_t, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *, uint8_t *, uint8_t *, uint16_t, uint32_t);
extern UART_HandleTypeDef huart1, huart2; extern I2C_HandleTypeDef hi2c1; extern SPI_HandleTypeDef hspi1;
