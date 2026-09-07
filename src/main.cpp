#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <arduino_homekit_server.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include "PinConfig.h"
#include "Debug.h"
#include "IrManager.h"
#include "SensorManager.h"
#include "LedManager.h"
#include "TimerManager.h"
#include "MqttManager.h"
#include "OtaManager.h"
#include "CommandHandler.h"
#include "DeviceConfig.h"
#include "WifiManagerEx.h"

IrManager irManager;
SensorManager sensorManager;
LedManager ledManager;
TimerManager timerManager;
MqttManager mqttManager;

WifiManagerEx wifiManager;

IRac ac(IR_TX_PIN);
String lastProtocolName = "KELVINATOR";
float envTemperature = 25.0;
float enHumidity = 50.0;
int acTargetTemp = 26;
int acTargetMode = 0;   // 0=关 1=制热 2=制冷 3=自动
int acFanSpeed = 2;     // 0=自动 1=低速 2=中速 3=高速 4=最大
bool acPower = false;
bool fanSwitch = true;  // 风扇开关（独立于空调电源）

// 风速档位 -> HomeKit 百分比（0/25/50/75/100）
static float fanSpeedToPct(int s)
{
    switch (s)
    {
        case 0: return 0.0f;
        case 1: return 25.0f;
        case 2: return 50.0f;
        case 3: return 75.0f;
        default: return 100.0f;
    }
}

// HomeKit accessory（定义于 my_accessory.c）
extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t cha_target_temperature;
extern "C" homekit_characteristic_t cha_target_heating_cooling_state;
extern "C" homekit_characteristic_t cha_current_temperature;
extern "C" homekit_characteristic_t cha_current_heating_cooling_state;
extern "C" homekit_characteristic_t cha_current_relative_humidity;
extern "C" homekit_characteristic_t cha_fan_active;
extern "C" homekit_characteristic_t cha_fan_rotation_speed;

String loadProtocolFromFS();

static bool otaKickoffPending = false;
static unsigned long otaKickoffAt = 0;
static String pendingOtaPublish = "";

// 重新配网/恢复出厂命令的延迟重启（先让 MQTT 把回复发出去）
bool recoveryRestartPending = false;
bool recoveryIsFactory = false;   // true=恢复出厂类（重启前白闪），false=重新配网
unsigned long recoveryRestartAt = 0;

static void stopBackgroundNetwork()
{
    if (mqttManager.isConnected())
        mqttManager.forceDisconnect();
}

static void queueOtaPublish(const String &payload)
{
    pendingOtaPublish = payload;
}

// BOOT 按键（GPIO2）长按：1.5s 黄灯等待恢复出厂，继续按住到 3s 执行
static unsigned long keyPressedAt = 0;
static bool keyArmed = false;

static void handleFactoryResetButton()
{
    if (digitalRead(KEY_PIN) == LOW)
    {
        if (keyPressedAt == 0)
            keyPressedAt = millis();
        unsigned long held = millis() - keyPressedAt;
        if (!keyArmed && held >= 1500)
        {
            keyArmed = true;
            ledManager.setSteady(CRGB::Yellow);   // 等待恢复出厂：黄色常亮
            Serial.println("[按键] 长按 1.5s，进入恢复出厂等待（继续按住到 3s 执行）");
        }
        if (keyArmed && held >= 3000)
        {
            keyArmed = false;
            keyPressedAt = 0;
            Serial.println("[按键] 执行恢复出厂设置");
            LittleFS.remove("/wifi.json");  // 清配网凭据
            ESP.eraseConfig();                // 清 SDK 旧凭据（重启后生效）
            homekit_storage_reset();          // 清 HomeKit 配对
            LittleFS.remove("/timers.txt");   // 清定时任务
            LittleFS.remove("/protocol.txt"); // 清协议配置
            recoveryIsFactory = true;
            recoveryRestartPending = true;
            recoveryRestartAt = millis();
        }
    }
    else
    {
        if (keyArmed)
        {
            keyArmed = false;
            ledManager.off();   // 未按满 3s，取消恢复出厂
        }
        keyPressedAt = 0;
    }
}

// HomeKit 目标状态 -> 发送红外
void hkSendAc()
{
    if (acTargetMode < 0 || acTargetMode > 3)
        acTargetMode = 2;   // 异常值兜底为制冷
    bool powerOn = (acTargetMode != 0);
    int sendMode;
    switch (acTargetMode)
    {
        case 0: sendMode = (int)stdAc::opmode_t::kOff; break;
        case 1: sendMode = (int)stdAc::opmode_t::kHeat; break;
        case 2: sendMode = (int)stdAc::opmode_t::kCool; break;
        default: sendMode = (int)stdAc::opmode_t::kAuto; break;
    }
    int sendSpeed = (acTargetMode == 3) ? 0 : acFanSpeed;
    irManager.send(acTargetTemp, sendSpeed, sendMode, powerOn);
    acPower = powerOn;
    DBG("HomeKit 控制: 温度=%d 模式=%d 风速=%d 电源=%s\n",
        acTargetTemp, acTargetMode, sendSpeed, powerOn ? "开" : "关");
}

extern "C" void hk_set_target_temperature(const homekit_value_t value)
{
    acTargetTemp = (int)(value.float_value + 0.5f);
    cha_target_temperature.value.float_value = value.float_value;
    hkSendAc();
}

extern "C" void hk_set_target_heating_cooling_state(const homekit_value_t value)
{
    int m = value.int_value;
    if (m < 0 || m > 3)
        m = 2;
    acTargetMode = m;
    cha_target_heating_cooling_state.value.int_value = m;
    hkSendAc();
}

// 风扇开关：只控制风扇状态显示，不影响空调电源
extern "C" void hk_set_fan_active(const homekit_value_t value)
{
    fanSwitch = (value.int_value != 0);
    cha_fan_active.value.int_value = fanSwitch ? 1 : 0;
    homekit_characteristic_notify(&cha_fan_active, cha_fan_active.value);
}

// 风扇转速 0-100% -> 空调 5 档（自动/低速/中速/高速/最大）
extern "C" void hk_set_fan_rotation_speed(const homekit_value_t value)
{
    int pct = (int)(value.float_value + 0.5f);
    if (pct <= 0) acFanSpeed = 0;        // 自动
    else if (pct <= 25) acFanSpeed = 1;  // 低速
    else if (pct <= 50) acFanSpeed = 2;  // 中速
    else if (pct <= 75) acFanSpeed = 3;  // 高速
    else acFanSpeed = 4;                 // 最大
    cha_fan_rotation_speed.value.float_value = fanSpeedToPct(acFanSpeed);
    homekit_characteristic_notify(&cha_fan_rotation_speed, cha_fan_rotation_speed.value);
    if (acTargetMode != 0)
        hkSendAc();   // 空调开着才重新发红外
}

void reportHomeKit()
{
    cha_current_temperature.value.float_value = envTemperature;
    homekit_characteristic_notify(&cha_current_temperature, cha_current_temperature.value);

    cha_current_relative_humidity.value.float_value = enHumidity;
    homekit_characteristic_notify(&cha_current_relative_humidity, cha_current_relative_humidity.value);

    int curState = acPower ? (acTargetMode == 1 ? 1 : 2) : 0;
    cha_current_heating_cooling_state.value.int_value = curState;
    homekit_characteristic_notify(&cha_current_heating_cooling_state, cha_current_heating_cooling_state.value);

    cha_fan_active.value.int_value = fanSwitch ? 1 : 0;
    homekit_characteristic_notify(&cha_fan_active, cha_fan_active.value);
    cha_fan_rotation_speed.value.float_value = fanSpeedToPct(acFanSpeed);
    homekit_characteristic_notify(&cha_fan_rotation_speed, cha_fan_rotation_speed.value);
}

void setup()
{
    Serial.begin(115200);
    if (!LittleFS.begin())
        Serial.println("LittleFS 初始化失败");
    otaManager.begin();

    ledManager.begin();
    ledManager.off();   // 上电初始化：熄灭

    pinMode(KEY_PIN, INPUT_PULLUP);   // BOOT 按键（按下为低）

    sensorManager.begin();
    irManager.begin();
    timerManager.begin();

    lastProtocolName = loadProtocolFromFS();
    updateProtocolFromString(lastProtocolName);

    ac.next.model = 1;
    ac.next.celsius = true;
    ac.next.degrees = 25;
    ac.next.mode = stdAc::opmode_t::kCool;   // 初始化模式，避免状态上报 mode=-1
    ac.next.fanspeed = stdAc::fanspeed_t::kMedium;
    ac.next.swingv = stdAc::swingv_t::kOff;
    ac.next.swingh = stdAc::swingh_t::kOff;
    ac.next.light = true;
    ac.next.beep = false;
    ac.next.econo = false;
    ac.next.filter = false;
    ac.next.turbo = false;
    ac.next.quiet = false;
    ac.next.sleep = -1;
    ac.next.clean = false;
    ac.next.clock = -1;
    ac.next.power = false;

    wifiManager.begin();   // 非阻塞：有凭据静默连，无凭据直接开配网热点
    wifiManager.enable();

    // HomeKit：setter 必须在 arduino_homekit_setup 之前挂接
    cha_target_temperature.setter = hk_set_target_temperature;
    cha_target_heating_cooling_state.setter = hk_set_target_heating_cooling_state;
    cha_target_temperature.value.float_value = acTargetTemp;
    cha_target_heating_cooling_state.value.int_value = acTargetMode;
    cha_current_temperature.value.float_value = envTemperature;
    cha_current_relative_humidity.value.float_value = enHumidity;
    cha_fan_active.value.int_value = fanSwitch ? 1 : 0;
    cha_fan_rotation_speed.value.float_value = fanSpeedToPct(acFanSpeed);
    cha_fan_active.setter = hk_set_fan_active;
    cha_fan_rotation_speed.setter = hk_set_fan_rotation_speed;
    arduino_homekit_setup(&config);
    Serial.println("HomeKit 已启动，配对码 111-11-111");

    mqttManager.begin();
    Serial.println("系统初始化完成");
}

void loop()
{
    if (recoveryRestartPending)
    {
        if (recoveryIsFactory)
            ledManager.blinkWhite();   // 恢复出厂：白色闪烁（重启前）
        if (millis() - recoveryRestartAt >= 800)
        {
            recoveryRestartPending = false;
            recoveryIsFactory = false;
            ESP.restart();
        }
    }

    handleFactoryResetButton();

    wifiManager.loop();   // 配网页/重连退避/热点管理（每轮都跑）

    if (otaManager.consumeDownloadRequest())
    {
        stopBackgroundNetwork();
        otaKickoffPending = true;
        otaKickoffAt = millis();
    }

    if (otaKickoffPending && millis() - otaKickoffAt >= 500)
    {
        otaKickoffPending = false;
        String otaErr = "";
        if (!otaManager.beginDownload(otaManager.getUrl(), otaErr))
        {
            queueOtaPublish(String("ota=fail:") + otaErr);
        }
    }

    if (!otaKickoffPending && !otaManager.isDownloading())
    {
        arduino_homekit_loop();

        // HomeKit 配对/连接期间暂停 MQTT，释放堆内存
        // （SRP 加密运算需要较多空闲堆，内存不足会报 code -2 导致配对失败），
        // 客户端断开后自动恢复。
        static bool homekitPauseActive = false;
        const bool homeKitClientActive = arduino_homekit_connected_clients_count() > 0;
        // 只在未配对时暂停 MQTT（配对需要腾内存）。配对成功后日常控制不再暂停，
        // 否则 HomeKit 客户端长期连接会导致 MQTT 一直暂停（网页面板用不了）
        const bool needPause = homeKitClientActive && !homekit_is_paired();
        if (needPause)
        {
            if (!homekitPauseActive)
            {
                homekitPauseActive = true;
                mqttManager.releaseMemoryForPairing();   // 断开 MQTT + 缓冲缩到 64B，腾堆给 SRP
                Serial.printf("[HomeKit] 客户端连接（未配对），暂停 MQTT，heap=%u maxBlock=%u\n",
                              ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
            }
        }
        else
        {
            if (homekitPauseActive)
            {
                homekitPauseActive = false;
                mqttManager.restoreMemoryForNormal();   // 恢复 MQTT 缓冲，loop 自动重连
                Serial.printf("[HomeKit] 已配对或客户端断开，恢复 MQTT，heap=%u\n", ESP.getFreeHeap());
            }
            mqttManager.loop();
        }
        if (pendingOtaPublish.length() > 0 && mqttManager.isConnected())
        {
            mqttManager.publish(pendingOtaPublish);
            pendingOtaPublish = "";
        }
    }

    if (otaManager.consumeRestart())
    {
        delay(500);
        ESP.restart();
    }

    String otaErr = "";
    int pct = -1;
    int st = otaManager.processDownload(otaErr, pct);
    if (st == OTA_DL_DONE)
    {
        ledManager.setSteady(CRGB::Green);   // OTA 成功，重启前绿色常亮
        mqttManager.publish("ota=idle");
        mqttManager.publish("ota=ok");
        delay(300);
        ESP.restart();
    }
    else if (st == OTA_DL_ERROR)
    {
        ledManager.blinkRed();   // OTA 失败：红色闪烁
        queueOtaPublish(String("ota=fail:") + otaErr);
    }

    if (otaKickoffPending || otaManager.isDownloading())
    {
        ledManager.blinkWhite();   // OTA 下载中：白色闪烁
        delay(1);
        yield();
        return;
    }

    sensorManager.loop();
    timerManager.loop();
    irManager.loop();
    ledManager.update();

    static unsigned long lastHk = 0;
    if (millis() - lastHk > 10000)
    {
        lastHk = millis();
        reportHomeKit();
    }
}
