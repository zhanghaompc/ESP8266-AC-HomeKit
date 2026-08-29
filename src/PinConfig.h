#pragma once
// ESP8266 GPIO 引脚定义（按实际硬件调整）
#define KEY_PIN     0   // BOOT 按键
#define IR_TX_PIN   14   // 红外发射
#define IR_RX_PIN   12  // 红外接收
#define LED_PIN     2   // WS2812B RGB 状态灯
#define AHT_SDA_PIN 4   // AHT20 I2C SDA
#define AHT_SCL_PIN 5  // AHT20 I2C SCL
#define WEB_PORT    8080 // 本地网页端口（HomeKit 占用 80）
