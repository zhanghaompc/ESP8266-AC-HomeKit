#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <base64.h>
#include <ESP8266WebServer.h>

class WebServerEx
{
public:
    WebServerEx();
    void begin();   // WiFiManager 配网 + 启动 8080 网页服务
    void loop();
    bool isConnected() const;
    void stop();

private:
    ESP8266WebServer server;
    bool webActive = false;
    bool wifiConnected = false;
    void setupHandlers();
    void connectWiFi();
};
