#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include "PinConfig.h"

// ============================================================
// WiFi 管理（移植自 ESP32 版 WifiManagerEx，适配 ESP8266）
//
// 连接策略（STA 与 AP 分工明确）：
//   1. 有凭据时先静默重连，退避间隔 5/10/20/30 秒封顶，此阶段不开 AP
//   2. 连续失败超过 staGraceMs 才拉起配网 AP（AP_STA 模式，STA 继续重试）
//   3. STA 连上后关闭 AP —— AP_STA 长期共存会拖累 STA 吞吐
//   4. 没有任何凭据时直接开 AP 等配置
//
// 配网门户：端口 80（8266 的 HomeKit HAP 实际监听 5556，80 空闲），
//   captive portal 把任意域名重定向到 http://192.168.4.1
// 凭据存储：LittleFS /wifi.json（ArduinoJson）
// ============================================================

class WifiManagerEx
{
public:
    WifiManagerEx();
    void begin();
    void enable();
    void disable();
    void loop();
    bool isConnected() const;
    // 配网门户是否开启中（开启期间应暂停可能抢射频/端口的操作）
    bool isConfigPortalActive() const { return configPortalActive; }
    void startConfigPortal();

private:
    ESP8266WebServer configServer; // 配网门户 :80
    DNSServer dnsServer;           // captive portal：全部域名指向 AP IP

    bool wifiConnected = false;
    bool configPortalActive = false;
    bool configHandlersReady = false;
    bool hasCredentials = false;
    bool radioEnabled = false;

    // STA 重连退避：失败次数越多间隔越长，最长 30 秒
    uint8_t retryCount = 0;
    unsigned long lastStaAttemptTime = 0;
    unsigned long staDownSince = 0;
    // 断连多久后才开配网热点（这段时间留给静默重连）
    static const unsigned long staGraceMs = 30000;
    static const unsigned long apLingerMs = 1500;

    // 异步扫描状态：避免阻塞扫描打断 AP 广播导致手机掉线
    int scanState = -2; // -2=未开始 -1=进行中 >=0=结果数
    unsigned long scanStartTime = 0;
    static const unsigned long scanTimeoutMs = 15000;
    // 扫描期间临时暂停 STA：ESP8266 单射频，STA 连接/重连中会拒绝扫描
    bool staPausedForScan = false;

    String pendingSsid;
    String pendingPass;

    void setupConfigPortalHandlers();
    void checkWiFiConnection();
    void beginStaConnect();
    unsigned long currentBackoff() const;
    void startAccessPoint();
    void resetWifiStack();
    void stopConfigPortal();
    void connectWiFi();
    void handleScanRequest();
    void resumeStaAfterScan();
    bool loadWifiCredentials(String &ssid, String &pass);
    void saveWifiCredentials(const String &ssid, const String &pass);
    void sendConfigPage();
};
