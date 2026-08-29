#pragma once
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include "PinConfig.h"

class IrManager
{
public:
    IrManager();
    void begin();
    void loop();   // 在主循环中调用，处理待发送的红外指令
    void send(int temp, int speed, int mode, bool power = true);
    // 红外接收 / 协议学习
    void beginLearn();                 // 开始学习（紫灯）
    void stopLearning();
    String getLearnedProtocol() const; // 学习到的协议名，空=还没学到
    void processReceive();             // 解码接收数据（普通模式打印，学习模式捕获）

private:
    volatile bool pending = false;
    int pTemp = 25;
    int pSpeed = 2;
    int pMode = 1;
    bool pPower = true;
    IRrecv irrecv = IRrecv(IR_RX_PIN, 1024, 50, true);
    decode_results results;
    bool learning = false;
    String learnedProtocol = "";
};
