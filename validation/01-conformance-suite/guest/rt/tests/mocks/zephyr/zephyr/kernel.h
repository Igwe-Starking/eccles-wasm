#pragma once
#include <stdint.h>
#include <stddef.h>
struct device { int x; };
#define DT_NODELABEL(x) x
#define gpio0_status 1
#define gpio1_status 0
#define gpio2_status 0
#define gpio3_status 0
#define DT_PASTE_(a, b) a##b
#define DT_PASTE(a, b) DT_PASTE_(a, b)
#define DT_NODE_HAS_STATUS(n, s) DT_PASTE(n, _status)
#define DEVICE_DT_GET(n) (&dev_gpio0)
extern const struct device dev_gpio0;
int device_is_ready(const struct device *);
uint32_t k_uptime_get_32(void); int64_t k_uptime_ticks(void); int64_t k_ticks_to_us_floor64(int64_t);
int32_t k_msleep(int32_t); void k_busy_wait(uint32_t); void k_yield(void);
