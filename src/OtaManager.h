#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

class HTTPClient;

// 检查更新结果
enum OtaCheckResult
{
    OTA_CHECK_FAILED = 0,     // 检查失败
    OTA_CHECK_NO_UPDATE = 1,  // 已是最新版本
    OTA_CHECK_OK = 2          // 发现新版本
};

// 异步下载状态（配合主循环分片读取，可上报进度）
enum OtaDownloadStatus
{
    OTA_DL_IDLE = 0,
    OTA_DL_RUNNING = 1,
    OTA_DL_DONE = 2,
    OTA_DL_ERROR = 3
};

class OtaManager
{
public:
    void begin();                          // 加载配置（LittleFS）
    String getUrl() const;
    String getVersion() const;
    String getPendingUrl() const;
    bool setUrl(const String &url);
    int checkForUpdate(String &remoteVersion, String &errMsg); // 只查版本，不下载
    bool beginDownload(const String &downloadUrl, String &errMsg); // 开始异步下载
    int processDownload(String &errMsg, int &progressPercent);    // 主循环分片驱动
    bool isDownloading() const;
    bool requestDownload();
    bool consumeDownloadRequest();
    bool isNewer(const String &remote) const;        // 远端版本是否比本地新
    void requestRestart();
    bool consumeRestart();
    bool refreshManifest(String &remoteVersion, String &remoteUrl, String &errMsg);

private:
    String url = "";
    String pendingUrl = "";
    HTTPClient *dlHttp = nullptr;
    WiFiClient *dlPlain = nullptr;
    WiFiClientSecure *dlSecure = nullptr;
    bool downloading = false;
    bool downloadRequested = false;
    unsigned long totalRead = 0;
    unsigned long lastDataMs = 0;
    int contentLength = -1;
    bool restartPending = false;
    void saveConfigFile();
    void finishDownload();
    bool fetchMetadata(String &remoteVersion, String &remoteUrl, String &errMsg);
    bool isVersionNewer(const String &remote, const String &current) const;
};

extern OtaManager otaManager;
