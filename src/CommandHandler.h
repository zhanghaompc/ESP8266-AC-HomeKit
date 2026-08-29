#pragma once
#include <Arduino.h>

// 统一指令处理（MQTT / 网页共用），返回响应字符串
String handleCommand(const String &cmd);

bool updateProtocolFromString(const String &protocolName);
String getProtocolName();
