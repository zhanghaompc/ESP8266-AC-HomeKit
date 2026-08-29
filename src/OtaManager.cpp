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
        bool isHttps = curUrl.startsWith("https://");
        if (isHttps)
        {
            dlSecure = new WiFiClientSecure();
            dlSecure->setInsecure();   // 跳过证书校验（家用可接受）
            dlSecure->setBufferSizes(2048, 512); // 减小 TLS 缓冲，ESP8266 内存有限
            dlHttp = new HTTPClient();
            dlHttp->setTimeout(45000);
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
    uint8_t buf[1024];
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
    for (int attempt = 1; attempt <= 2; attempt++)
    {
        String curUrl = url;
        int slash = curUrl.lastIndexOf('/');
        if (slash < 0)
        {
            errMsg = "OTA url invalid";
            return false;
        }
        String metaUrl = curUrl.substring(0, slash + 1) + "ota.json";
        DBG("[OTA] 读取版本清单 %s\n", metaUrl.c_str());

        bool isHttps = metaUrl.startsWith("https://");
        WiFiClient plainClient;
        WiFiClientSecure secureClient;
        if (isHttps)
            secureClient.setBufferSizes(2048, 512);
        HTTPClient http;
        http.setTimeout(30000);
        bool beginOk;
        if (isHttps)
        {
            secureClient.setInsecure();
            beginOk = http.begin(secureClient, metaUrl);
        }
        else
        {
            beginOk = http.begin(plainClient, metaUrl);
        }
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
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream()) != DeserializationError::Ok)
        {
            errMsg = "meta JSON 解析失败";
            http.end();
            continue;
        }
        remoteVersion = doc["version"] | "";
        remoteUrl = doc["url"] | "";
        http.end();
        if (remoteVersion.length() == 0)
        {
            errMsg = "meta 缺少 version 字段";
            continue;
        }
        return true;
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

