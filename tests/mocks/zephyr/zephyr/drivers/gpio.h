#pragma once
#include <zephyr/kernel.h>
typedef uint8_t gpio_pin_t; typedef uint32_t gpio_flags_t;
#define GPIO_INPUT 1u
#define GPIO_OUTPUT 2u
#define GPIO_PULL_UP 4u
#define GPIO_PULL_DOWN 8u
#define GPIO_OPEN_DRAIN 16u
int gpio_pin_configure(const struct device *, gpio_pin_t, gpio_flags_t);
int gpio_pin_set_raw(const struct device *, gpio_pin_t, int);
int gpio_pin_get_raw(const struct device *, gpio_pin_t);
