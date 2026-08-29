#pragma once
#include <Arduino.h>

// 固件版本号（OTA 状态显示用）
#define FW_VERSION "1.0.7"

// ============================================================
// 设备编号：基于 ESP8266 ChipID 的后 4 位十六进制
// 用于生成唯一的 WiFi 热点名和 MQTT 主题前缀
// 例：热点 ESP8266AC_a1b2，主题 ac/esp8266aca1b2（后缀统一小写）
// ============================================================

inline String deviceSuffix()
{
    static String s;
    if (s.length() == 0)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%04x", (uint32_t)(ESP.getChipId() & 0xFFFF));
        s = buf;
    }
    return s;
}

// WiFi 热点名：ESP8266AC_xxxx（小写，与 MQTT 主题后缀一致）
inline String deviceApName()
{
    return "ESP8266AC_" + deviceSuffix();
}

// MQTT 主题前缀：ac/esp8266acxxxx（小写）
inline String deviceMqttBase()
{
    return "ac/esp8266ac" + deviceSuffix();
}

