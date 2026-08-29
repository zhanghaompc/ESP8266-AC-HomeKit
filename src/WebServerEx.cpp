#include "WebServerEx.h"
#include "MqttManager.h"
#include "CommandHandler.h"
#include "TimerManager.h"
#include "LedManager.h"
#include "PinConfig.h"
#include "OtaManager.h"
#include "Debug.h"
#include "DeviceConfig.h"
#include <WiFiManager.h>
#include <arduino_homekit_server.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <Updater.h>

extern LedManager ledManager;
extern TimerManager timerManager;
extern IRac ac;
extern float envTemperature;
extern float enHumidity;
extern String lastProtocolName;
extern OtaManager otaManager;

WebServerEx::WebServerEx() : server(WEB_PORT) {}

void WebServerEx::begin()
{
    connectWiFi();
    setupHandlers();
    server.begin();
    webActive = true;
    Serial.printf("Web 服务已启动: http://%s:%d\n", WiFi.localIP().toString().c_str(), WEB_PORT);
}

void WebServerEx::loop()
{
    if (webActive)
        server.handleClient();
    if (WiFi.status() != WL_CONNECTED)
    {
        static unsigned long lastAttempt = 0;
        if (millis() - lastAttempt > 5000)
        {
            lastAttempt = millis();
            WiFi.reconnect();
            ledManager.blinkGreen();
        }
    }
    else
    {
        wifiConnected = true;
        ledManager.stopBlink();
        ledManager.setSteady(CRGB::Blue);
    }
}

bool WebServerEx::isConnected() const { return wifiConnected; }

void WebServerEx::stop()
{
    server.stop();
    webActive = false;
}

void WebServerEx::connectWiFi()
{
    WiFiManager wifiManager;
    wifiManager.setTimeout(120);
    String apName = deviceApName();   // 例如 ESP8266AC_a1b2
    wifiManager.setTitle(("设备配网 " + apName).c_str());

    // 配网页面上显示设备编号和推荐 MQTT 主题（小写，直接复制即可）
    String deviceHtml = "<div style='padding:8px 0;font-size:14px;color:#666'>"
                        "设备编号：<b style='color:#222'>" + apName + "</b><br>"
                        "MQTT主题：<b style='color:#222'>" + deviceMqttBase() + "</b></div>";
    WiFiManagerParameter deviceParam(deviceHtml.c_str());
    wifiManager.addParameter(&deviceParam);

    ledManager.blinkGreen();
    if (!wifiManager.autoConnect(apName.c_str()))
    {
        Serial.println("配网超时，继续尝试连接已有 WiFi");
        WiFi.reconnect();
    }
    Serial.printf("WiFi 已连接: %s\n", WiFi.localIP().toString().c_str());
    ledManager.stopBlink();
    ledManager.setSteady(CRGB::Blue);
}

void WebServerEx::setupHandlers()
{
    server.on("/", HTTP_GET, [this]() {
        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESP8266 AC</title></head>"
                      "<body style='font-family:sans-serif;padding:20px'>"
                      "<h2>ESP8266 空调控制器</h2>"
                      "<p><a href='/set?temp=26&mode=1&speed=2'>开机 26°C 制冷中速</a></p>"
                      "<p><a href='/set?temp=26&mode=2&speed=2'>开机 26°C 制热中速</a></p>"
                      "<p><a href='/power'>电源开关</a></p>"
                      "<p><a href='/sensor'>传感器</a></p>"
                      "<p><a href='/mqttget'>MQTT 配置</a></p>"
                      "<p><a href='/timers'>定时任务</a></p>"
                      "<p><a href='/hkreset' onclick=\"return confirm('确定清除 HomeKit 配对记录？清除后需要重新配对 111-11-111')\">重置 HomeKit 配对</a></p>"
                      "<p>MQTT 设置: /mqttset?host=&port=&user=&pass=&topic=</p>"
                      "</body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/hkreset", HTTP_GET, [this]() {
        server.send(200, "text/plain", "HomeKit pairing reset, rebooting...");
        delay(200);
        homekit_storage_reset();
        delay(200);
        ESP.restart();
    });

    server.on("/set", HTTP_GET, [this]() {
        String temp = server.arg("temp");
        String mode = server.arg("mode");
        String speed = server.arg("speed");
        String protocol = server.arg("protocol");
        if (!protocol.isEmpty())
            updateProtocolFromString(protocol);
        String resp = handleCommand("temp=" + temp + ";mode=" + mode + ";speed=" + speed + ";power=on");
        server.send(200, "text/plain", resp);
    });

    server.on("/protocol", HTTP_GET, [this]() {
        server.send(200, "text/plain", lastProtocolName);
    });

    server.on("/sensor", HTTP_GET, [this]() {
        String json = "{\"temp\":" + String(envTemperature, 1) + ",\"humidity\":" + String(enHumidity, 1) + "}";
        server.send(200, "application/json", json);
    });

    server.on("/power", HTTP_GET, [this]() {
        static bool powerState = false;
        powerState = !powerState;
        String resp = powerState ? handleCommand("temp=26;mode=1;speed=2;power=on") : handleCommand("power=off");
        server.send(200, "text/plain", resp);
    });

    server.on("/timers", HTTP_GET, [this]() {
        server.send(200, "application/json", timerManager.getTaskList());
    });

    server.on("/mqttget", HTTP_GET, [this]() {
        server.send(200, "application/json", mqttManager.getConfigJson());
    });

    server.on("/mqttset", HTTP_GET, [this]() {
        String host = server.arg("host");
        String port = server.arg("port");
        String user = server.arg("user");
        String pass = server.arg("pass");
        String topic = server.arg("topic");
        mqttManager.setConfig(host, port.isEmpty() ? 1883 : (uint16_t)port.toInt(), user, pass, topic);
        server.send(200, "text/plain", "MQTT config saved: " + mqttManager.getConfigJson());
    });

    server.on("/reboot", HTTP_GET, [this]() {
        server.send(200, "text/plain", "reboot");
        delay(200);
        ESP.restart();
    });

    // OTA：查询当前固件版本和升级地址
    server.on("/otaget", HTTP_GET, [this]() {
        String json = "{\"fw\":\"" + otaManager.getVersion() +
                      "\",\"url\":\"" + otaManager.getUrl() + "\"}";
        server.send(200, "application/json", json);
    });

    // OTA：只检查版本（不下载）
    server.on("/otacheck", HTTP_GET, [this]() {
        String ver = "", err = "";
        int ret = otaManager.checkForUpdate(ver, err);
        String json = "{\"fw\":\"" + otaManager.getVersion() + "\",\"latest\":\"" +
                      (ret == OTA_CHECK_OK ? ver : otaManager.getVersion()) +
                      "\",\"update\":" + String(ret == OTA_CHECK_OK ? "true" : "false") + "}";
        server.send(200, "application/json", json);
    });

    // OTA：设置升级地址
    server.on("/otaset", HTTP_GET, [this]() {
        String u = server.arg("url");
        bool ok = otaManager.setUrl(u);
        server.send(200, "text/plain", ok ? "OTA URL saved" : "invalid url");
    });

    // OTA：网页上传固件升级
    server.on("/update", HTTP_GET, [this]() {
        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<title>固件升级</title></head>"
                      "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                      "<h2>固件升级 (OTA)</h2>"
                      "<form method='POST' action='/update' enctype='multipart/form-data'>"
                      "<input type='file' name='firmware' accept='.bin'><br><br>"
                      "<button type='submit'>上传并升级</button></form>"
                      "<p style='color:#888;font-size:12px'>升级期间请勿断电，完成后设备自动重启</p>"
                      "</body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/update", HTTP_POST, [this]() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", Update.hasError() ? "升级失败" : "升级成功，正在重启...");
        delay(1000);
        ESP.restart();
    }, [this]() {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START)
        {
            Serial.printf("OTA 上传开始: %s\n", upload.filename.c_str());
            uint32_t maxSize = ESP.getFreeSketchSpace();
            if (!Update.begin(maxSize))
                Update.printError(Serial);
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
                Update.printError(Serial);
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
            if (Update.end(true))
                Serial.printf("OTA 成功，重启中... (%d 字节)\n", upload.totalSize);
            else
                Update.printError(Serial);
        }
    });
}
