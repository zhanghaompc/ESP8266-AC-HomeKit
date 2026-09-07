#include "OtaManager.h"
#include "DeviceConfig.h"
#include "Debug.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>

#define OTA_CONFIG_FILE "/ota.json"
// ESP8266 固件放在 firmware/esp8266/ 下，直接使用 HTTP 拉取，不再走 MQTT 分包升级
#define OTA_DEFAULT_URL "https://fastly.jsdelivr.net/gh/zhanghaompc/ESP8266-AC-HomeKit@master/firmware/esp8266/esp8266_wifi.bin"

// 按环境取清单字段（与 ESP32 OTA 逻辑对齐）
#ifndef OTA_ENV_NAME
#define OTA_ENV_NAME "esp8266"
#endif

// 多源清单：CDN 主源 → CDN 备用 → raw 直连；master 分支带时间戳防 CDN 缓存
static const char *OTA_MANIFEST_BASES[] = {
    "https://cdn.jsdelivr.net/gh/zhanghaompc/ESP8266-AC-HomeKit@master",
    "https://fastly.jsdelivr.net/gh/zhanghaompc/ESP8266-AC-HomeKit@master",
    "https://raw.githubusercontent.com/zhanghaompc/ESP8266-AC-HomeKit/master"
};

// 清单字段支持字符串或按环境对象（如 {"esp8266": "1.0.14"} 或 {"default": "..."}）
static bool extractManifestString(JsonDocument &doc, const char *field, const char *env, String &out)
{
    JsonVariantConst node = doc[field];
    if (node.is<const char *>())
    {
        out = node.as<const char *>();
        return out.length() > 0;
    }

    if (node.is<JsonObjectConst>())
    {
        JsonObjectConst obj = node.as<JsonObjectConst>();
        if (obj[env].is<const char *>())
        {
            out = obj[env].as<const char *>();
            return out.length() > 0;
        }
        if (obj["default"].is<const char *>())
        {
            out = obj["default"].as<const char *>();
            return out.length() > 0;
        }
    }

    return false;
}


void OtaManager::begin()
{
    if (!LittleFS.exists(OTA_CONFIG_FILE))
    {
        url = OTA_DEFAULT_URL;
        saveConfigFile();
    }
    else
    {
        File f = LittleFS.open(OTA_CONFIG_FILE, "r");
        if (!f)
        {
            url = OTA_DEFAULT_URL;
        }
        else
        {
            JsonDocument doc;
            if (deserializeJson(doc, f) == DeserializationError::Ok)
                url = doc["url"] | OTA_DEFAULT_URL;
            f.close();
        }
    }

    // 早期默认域名 cdn.jsdelivr.net 国内不稳定，自动迁移到 fastly
    if (url.indexOf("cdn.jsdelivr.net") >= 0)
    {
        DBG("[OTA] 迁移 OTA 地址到 fastly 节点\n");
        url.replace("cdn.jsdelivr.net", "fastly.jsdelivr.net");
        saveConfigFile();
    }

    // 旧仓库迁移：从 ESP32 主仓库切到 ESP8266 独立仓库
    if (url.indexOf("ESP32-AC-HomeKit-IR") >= 0)
    {
        DBG("[OTA] 迁移 OTA 地址到 ESP8266 独立仓库\n");
        url.replace("zhanghaompc/ESP32-AC-HomeKit-IR", "zhanghaompc/ESP8266-AC-HomeKit");
        saveConfigFile();
    }
}

void OtaManager::saveConfigFile()
{
    JsonDocument doc;
    doc["url"] = url;
    File f = LittleFS.open(OTA_CONFIG_FILE, "w");
    if (f)
    {
        serializeJson(doc, f);
        f.close();
    }
}

String OtaManager::getUrl() const { return url; }

String OtaManager::getVersion() const { return FW_VERSION; }

String OtaManager::getPendingUrl() const { return pendingUrl; }

bool OtaManager::isDownloading() const { return downloading; }

bool OtaManager::requestDownload()
{
    if (downloadRequested || downloading)
        return false;
    downloadRequested = true;
    return true;
}

bool OtaManager::consumeDownloadRequest()
{
    bool r = downloadRequested;
    downloadRequested = false;
    return r;
}

bool OtaManager::isNewer(const String &remote) const
{
    return isVersionNewer(remote, FW_VERSION);
}

void OtaManager::requestRestart() { restartPending = true; }

bool OtaManager::consumeRestart()
{
    bool r = restartPending;
    restartPending = false;
    return r;
}

bool OtaManager::refreshManifest(String &remoteVersion, String &remoteUrl, String &errMsg)
{
    return fetchMetadata(remoteVersion, remoteUrl, errMsg);
}

bool OtaManager::setUrl(const String &u)
{
    if (u.length() < 10 || !u.startsWith("http"))
        return false;
    url = u;
    saveConfigFile();
    return true;
}

void OtaManager::useLatestUrl()
{
    if (url == OTA_DEFAULT_URL)
        return;
    url = OTA_DEFAULT_URL;
    saveConfigFile();
}

int OtaManager::checkForUpdate(String &remoteVersion, String &errMsg)
{
    if (url.length() == 0)
    {
        errMsg = "OTA url is empty";
        return OTA_CHECK_FAILED;
    }

    String remoteUrl;
    if (!fetchMetadata(remoteVersion, remoteUrl, errMsg))
        return OTA_CHECK_FAILED;

    DBG("[OTA] 本地版本 %s，远端版本 %s\n", FW_VERSION, remoteVersion.c_str());
    if (!isVersionNewer(remoteVersion, FW_VERSION))
    {
        errMsg = "已是最新版本 " + String(FW_VERSION);
        return OTA_CHECK_NO_UPDATE;
    }

    pendingUrl = (remoteUrl.length() > 0) ? remoteUrl : url;
    return OTA_CHECK_OK;
}

bool OtaManager::beginDownload(const String &downloadUrl, String &errMsg)
{
    finishDownload();

    int code = -1;
    for (int attempt = 1; attempt <= 2; attempt++)
    {
        String curUrl = downloadUrl;
        bool isMovingBranchUrl =
            (curUrl.indexOf("raw.githubusercontent.com/") >= 0 && curUrl.indexOf("/master/") >= 0) ||
            (curUrl.indexOf("fastly.jsdelivr.net/") >= 0 && curUrl.indexOf("@master/") >= 0);
        if (isMovingBranchUrl && curUrl.indexOf('?') < 0)
        {
            curUrl += "?ota=" + String(ESP.getChipId(), HEX) + "-" + String(millis());
        }
        bool isHttps = curUrl.startsWith("https://");
        if (isHttps)
        {
            dlSecure = new WiFiClientSecure();
            dlSecure->setInsecure();   // 跳过证书校验（家用可接受）
            dlSecure->setBufferSizes(512, 512); // OTA 时尽量压低 TLS 内存占用
            dlHttp = new HTTPClient();
            dlHttp->setTimeout(45000);
            dlHttp->setReuse(false);
            if (!dlHttp->begin(*dlSecure, curUrl))
            {
                errMsg = "HTTP begin failed";
                finishDownload();
                continue;
            }
        }
        else
        {
            dlPlain = new WiFiClient();
            dlHttp = new HTTPClient();
            dlHttp->setTimeout(30000);
            dlHttp->setReuse(false);
            if (!dlHttp->begin(*dlPlain, curUrl))
            {
                errMsg = "HTTP begin failed";
                finishDownload();
                continue;
            }
        }
        code = dlHttp->GET();
        if (code == HTTP_CODE_OK)
            break;
        errMsg = "HTTP GET " + String(code) + " heap=" + String(ESP.getFreeHeap());
        finishDownload();
        delay(50);
        yield();
    }

    if (code != HTTP_CODE_OK)
    {
        if (errMsg.length() == 0)
            errMsg = "HTTP GET failed";
        return false;
    }

    // ESP8266 的 Update.begin 必须传真实大小（不能传未知大小），fastly 会返回 Content-Length
    contentLength = dlHttp->getSize();
    if (contentLength <= 0)
    {
        errMsg = "缺少 Content-Length";
        finishDownload();
        return false;
    }

    if (!Update.begin((size_t)contentLength))
    {
        errMsg = "Update.begin err=" + String(Update.getError());
        finishDownload();
        return false;
    }

    downloading = true;
    totalRead = 0;
    lastDataMs = millis();
    DBG("[OTA] 开始下载: %s (%d 字节)\n", downloadUrl.c_str(), contentLength);
    return true;
}

int OtaManager::processDownload(String &errMsg, int &progressPercent)
{
    progressPercent = -1;
    if (!downloading || dlHttp == nullptr)
        return OTA_DL_IDLE;

    Stream &s = dlHttp->getStream();
    uint8_t buf[512];
    int got = 0;
    unsigned long t0 = millis();
    // 每轮尽量读满缓冲区，但最多 30ms，避免长期占用主循环
    while (got < (int)sizeof(buf) && millis() - t0 < 30)
    {
        int avail = s.available();
        if (avail <= 0)
            break;
        int toRead = (avail > (int)sizeof(buf) - got) ? (int)sizeof(buf) - got : avail;
        int r = s.readBytes(buf + got, toRead);
        if (r <= 0)
            break;
        got += r;
    }

    if (got > 0)
    {
        if (Update.write(buf, got) != (size_t)got)
        {
            errMsg = "Update.write err=" + String(Update.getError());
            finishDownload();
            return OTA_DL_ERROR;
        }
        totalRead += got;
        lastDataMs = millis();
    }

    // 已读满 Content-Length 即完成
    if (totalRead >= (unsigned long)contentLength)
    {
        if (!Update.end(true))
        {
            errMsg = "Update.end err=" + String(Update.getError());
            finishDownload();
            return OTA_DL_ERROR;
        }
        DBG("[OTA] 下载完成，共 %lu 字节\n", totalRead);
        finishDownload();
        return OTA_DL_DONE;
    }

    if (millis() - lastDataMs > 20000)
    {
        errMsg = "下载超时";
        finishDownload();
        return OTA_DL_ERROR;
    }

    if (contentLength > 0)
        progressPercent = (int)(totalRead * 100 / contentLength);
    return OTA_DL_RUNNING;
}

void OtaManager::finishDownload()
{
    downloading = false;
    if (dlHttp != nullptr)
    {
        dlHttp->end();
        delete dlHttp;
        dlHttp = nullptr;
    }
    if (dlPlain != nullptr)
    {
        delete dlPlain;
        dlPlain = nullptr;
    }
    if (dlSecure != nullptr)
    {
        delete dlSecure;
        dlSecure = nullptr;
    }
    totalRead = 0;
    contentLength = -1;
}

bool OtaManager::fetchMetadata(String &remoteVersion, String &remoteUrl, String &errMsg)
{
    // 版本检查多源依次尝试（对齐 ESP32 OTA 逻辑）：CDN 主源 → CDN 备用 → raw 直连
    for (size_t baseIndex = 0; baseIndex < sizeof(OTA_MANIFEST_BASES) / sizeof(OTA_MANIFEST_BASES[0]); baseIndex++)
    {
        // jsDelivr/Fastly 可能缓存 master 分支内容，追加时间戳确保每次检查拿到最新清单
        String metaUrl = String(OTA_MANIFEST_BASES[baseIndex]) + "/firmware/esp8266/ota.json";
        metaUrl += "?t=" + String(millis());
        DBG("[OTA] 读取版本清单 %s\n", metaUrl.c_str());

        bool isHttps = metaUrl.startsWith("https://");
        WiFiClient plainClient;
        WiFiClientSecure secureClient;
        if (isHttps)
            secureClient.setBufferSizes(512, 512);
        HTTPClient http;
        http.setTimeout(4000);   // 版本检查必须快速返回，避免阻塞 MQTT 心跳被 Broker 踢下线
        http.useHTTP10(true);    // HTTP/1.0 直传，不协商 chunked
        http.addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        http.addHeader("Pragma", "no-cache");
        bool beginOk;
        if (isHttps)
        {
            secureClient.setInsecure();
            beginOk = http.begin(secureClient, metaUrl);
        }
        else
            beginOk = http.begin(plainClient, metaUrl);
        if (!beginOk)
        {
            errMsg = "meta HTTP begin failed";
            continue;
        }
        int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            errMsg = "meta HTTP " + String(code) + " heap=" + String(ESP.getFreeHeap());
            http.end();
            continue;
        }
        int metaSize = http.getSize();
        if (metaSize > 8192)
        {
            errMsg = "meta 响应过大";
            http.end();
            continue;
        }
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream()) != DeserializationError::Ok)
        {
            errMsg = "meta JSON 解析失败";
            http.end();
            continue;
        }
        // 兼容新格式（versions/urls，支持按环境对象）与旧格式（version/url 字符串）
        remoteVersion = "";
        remoteUrl = "";
        extractManifestString(doc, "versions", OTA_ENV_NAME, remoteVersion);
        if (remoteVersion.length() == 0)
            extractManifestString(doc, "version", OTA_ENV_NAME, remoteVersion);
        extractManifestString(doc, "urls", OTA_ENV_NAME, remoteUrl);
        if (remoteUrl.length() == 0)
            extractManifestString(doc, "url", OTA_ENV_NAME, remoteUrl);
        http.end();
        if (remoteVersion.length() == 0)
        {
            errMsg = "meta 缺少 version 字段";
            continue;
        }
        return true;
    }

    // 多源都失败时，最后回退：从当前配置的固件 URL 推断同目录 ota.json
    int slash = url.lastIndexOf('/');
    if (slash > 0)
    {
        String metaUrl = url.substring(0, slash + 1) + "ota.json";
        DBG("[OTA] 回退读取版本清单 %s\n", metaUrl.c_str());
        bool isHttps = metaUrl.startsWith("https://");
        WiFiClient plainClient;
        WiFiClientSecure secureClient;
        if (isHttps)
            secureClient.setBufferSizes(512, 512);
        HTTPClient http;
        http.setTimeout(4000);
        bool beginOk;
        if (isHttps)
        {
            secureClient.setInsecure();
            beginOk = http.begin(secureClient, metaUrl);
        }
        else
            beginOk = http.begin(plainClient, metaUrl);
        if (beginOk && http.GET() == HTTP_CODE_OK)
        {
            JsonDocument doc;
            if (deserializeJson(doc, http.getStream()) == DeserializationError::Ok)
            {
                remoteVersion = doc["version"] | "";
                remoteUrl = doc["url"] | "";
                if (remoteVersion.length() > 0)
                {
                    http.end();
                    return true;
                }
            }
        }
        http.end();
        errMsg = "meta 缺少 version 字段";
    }
    return false;
}


bool OtaManager::isVersionNewer(const String &remote, const String &current) const
{
    int r[3] = {0, 0, 0}, c[3] = {0, 0, 0};
    sscanf(remote.c_str(), "%d.%d.%d", &r[0], &r[1], &r[2]);
    sscanf(current.c_str(), "%d.%d.%d", &c[0], &c[1], &c[2]);
    for (int i = 0; i < 3; i++)
    {
        if (r[i] != c[i])
            return r[i] > c[i];
    }
    return false;
}

OtaManager otaManager;

