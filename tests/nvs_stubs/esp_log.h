#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, f, ...) printf("E %s: " f "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, f, ...) printf("W %s: " f "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, f, ...) printf("I %s: " f "\n", tag, ##__VA_ARGS__)
