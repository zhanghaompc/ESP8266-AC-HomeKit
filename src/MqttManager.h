#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// MQTT 远程控制：主题 <topic>/in 收指令、<topic>/out 发响应、<topic>/status 发状态
class MqttManager
{
public:
    MqttManager();
    void begin();
    void loop();
    void publish(const String &payload);
    void publishStatus();
    bool isConnected();
    void forceDisconnect();
    void setConfig(const String &host, uint16_t port, const String &user, const String &pass, const String &topic);
    String getConfigJson();

private:
    WiFiClient wifiClient;
    PubSubClient client;
    String host = "";
    uint16_t port = 1883;
    String user = "";
    String pass = "";
    String topic = "";
    unsigned long lastStatusTime = 0;
    unsigned long lastConnectAttempt = 0;

    void loadConfig();
    void saveConfig();
    void connect();
    void handleMessage(char *topic, byte *payload, unsigned int length);
    static void onMessage(char *topic, byte *payload, unsigned int length);
};

extern MqttManager mqttManager;
