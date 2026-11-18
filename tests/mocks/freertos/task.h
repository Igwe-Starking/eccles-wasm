#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words, void *arg, UBaseType_t prio, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t t);
