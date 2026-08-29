#include "IrManager.h"
#include "LedManager.h"
#include "Debug.h"
#include <FastLED.h>

extern LedManager ledManager;
extern String lastProtocolName;
extern IRac ac;

IrManager::IrManager() {}

void IrManager::begin()
{
    Serial.println("红外发射器已初始化");
    irrecv.enableIRIn();
    Serial.println("红外接收器已开启 (GPIO 12)");
}

void IrManager::loop()
{
    processReceive();   // 红外接收：普通模式打印，学习模式捕获

    if (!pending)
        return;

    int temp = pTemp;
    int speed = pSpeed;
    int mode = pMode;
    bool power = pPower;
    pending = false;

    ledManager.stopBlink();
    ledManager.flash(CRGB::Red, 300);

    ac.next.degrees = temp;
    ac.next.fanspeed = (stdAc::fanspeed_t)speed;
    ac.next.mode = (stdAc::opmode_t)mode;
    ac.next.power = power;
    // 附加功能（强劲/扫风/面板灯/睡眠/自清洁等）保留当前状态，切换后不被重置

    DBG("准备发射红外: 温度=%d 风速=%d 模式=%d 电源=%s 协议=%s\n",
        temp, speed, mode, power ? "开" : "关", lastProtocolName.c_str());
    ac.sendAc();
    delay(100);
}

// ===================== 红外接收 / 协议学习 =====================

void IrManager::beginLearn()
{
    learning = true;
    learnedProtocol = "";
    irrecv.enableIRIn();
    ledManager.blinkPurple();   // 学习模式 = 紫灯
}

void IrManager::stopLearning()
{
    learning = false;
    ledManager.stopBlink();
}

String IrManager::getLearnedProtocol() const
{
    return learnedProtocol;
}

void IrManager::processReceive()
{
    if (irrecv.decode(&results))
    {
        String protoName = typeToString(results.decode_type);
        if (learning)
        {
            if (protoName != "UNKNOWN")
            {
                learnedProtocol = protoName;
                Serial.println("学习到协议: " + protoName);
            }
        }
        else
        {
            Serial.println("Protocol: " + protoName + " " + resultToHumanReadableBasic(&results));
        }
        irrecv.resume();
    }
}

void IrManager::send(int temp, int speed, int mode, bool power)
{
    pTemp = temp;
    pSpeed = speed;
    pMode = mode;
    pPower = power;
    pending = true;
}
