#include "CommandHandler.h"
#include "IrManager.h"
#include "TimerManager.h"
#include "OtaManager.h"
#include "Debug.h"
#include <map>
#include <LittleFS.h>
#include <IRremoteESP8266.h>
#include <IRac.h>

extern IrManager irManager;
extern TimerManager timerManager;
extern IRac ac;
extern float envTemperature;
extern float enHumidity;
extern String lastProtocolName;
extern OtaManager otaManager;

std::map<String, decode_type_t> protocolMap = {
    {"UNKNOWN", UNKNOWN}, {"UNUSED", UNUSED}, {"RC5", RC5}, {"RC6", RC6},
    {"NEC", NEC}, {"SONY", SONY}, {"PANASONIC", PANASONIC}, {"JVC", JVC},
    {"SAMSUNG", SAMSUNG}, {"WHYNTER", WHYNTER}, {"AIWA_RC_T501", AIWA_RC_T501},
    {"LG", LG}, {"SANYO", SANYO}, {"MITSUBISHI", MITSUBISHI}, {"DISH", DISH},
    {"SHARP", SHARP}, {"COOLIX", COOLIX}, {"DAIKIN", DAIKIN}, {"DENON", DENON},
    {"KELVINATOR", KELVINATOR}, {"SHERWOOD", SHERWOOD}, {"MITSUBISHI_AC", MITSUBISHI_AC},
    {"RCMM", RCMM}, {"SANYO_LC7461", SANYO_LC7461}, {"RC5X", RC5X}, {"GREE", GREE},
    {"PRONTO", PRONTO}, {"NEC_LIKE", NEC_LIKE}, {"ARGO", ARGO}, {"TROTEC", TROTEC},
    {"NIKAI", NIKAI}, {"RAW", RAW}, {"GLOBALCACHE", GLOBALCACHE},
    {"TOSHIBA_AC", TOSHIBA_AC}, {"FUJITSU_AC", FUJITSU_AC}, {"MIDEA", MIDEA},
    {"MAGIQUEST", MAGIQUEST}, {"LASERTAG", LASERTAG}, {"CARRIER_AC", CARRIER_AC},
    {"HAIER_AC", HAIER_AC}, {"MITSUBISHI2", MITSUBISHI2}, {"HITACHI_AC", HITACHI_AC},
    {"HITACHI_AC1", HITACHI_AC1}, {"HITACHI_AC2", HITACHI_AC2}, {"GICABLE", GICABLE},
    {"HAIER_AC_YRW02", HAIER_AC_YRW02}, {"WHIRLPOOL_AC", WHIRLPOOL_AC},
    {"SAMSUNG_AC", SAMSUNG_AC}, {"LUTRON", LUTRON}, {"ELECTRA_AC", ELECTRA_AC},
    {"PANASONIC_AC", PANASONIC_AC}, {"PIONEER", PIONEER}, {"LG2", LG2}, {"MWM", MWM},
    {"DAIKIN2", DAIKIN2}, {"VESTEL_AC", VESTEL_AC}, {"TECO", TECO},
    {"SAMSUNG36", SAMSUNG36}, {"TCL112AC", TCL112AC}, {"LEGOPF", LEGOPF},
    {"MITSUBISHI_HEAVY_88", MITSUBISHI_HEAVY_88}, {"MITSUBISHI_HEAVY_152", MITSUBISHI_HEAVY_152},
    {"DAIKIN216", DAIKIN216}, {"SHARP_AC", SHARP_AC}, {"GOODWEATHER", GOODWEATHER},
    {"INAX", INAX}, {"DAIKIN160", DAIKIN160}, {"NEOCLIMA", NEOCLIMA},
    {"DAIKIN176", DAIKIN176}, {"DAIKIN128", DAIKIN128}, {"AMCOR", AMCOR},
    {"DAIKIN152", DAIKIN152}, {"MITSUBISHI136", MITSUBISHI136}, {"MITSUBISHI112", MITSUBISHI112},
    {"HITACHI_AC424", HITACHI_AC424}, {"SONY_38K", SONY_38K}, {"EPSON", EPSON},
    {"SYMPHONY", SYMPHONY}, {"HITACHI_AC3", HITACHI_AC3}, {"DAIKIN64", DAIKIN64},
    {"AIRWELL", AIRWELL}, {"DELONGHI_AC", DELONGHI_AC}, {"DOSHISHA", DOSHISHA},
    {"MULTIBRACKETS", MULTIBRACKETS}, {"CARRIER_AC40", CARRIER_AC40},
    {"CARRIER_AC64", CARRIER_AC64}, {"HITACHI_AC344", HITACHI_AC344},
    {"CORONA_AC", CORONA_AC}, {"MIDEA24", MIDEA24}, {"ZEPEAL", ZEPEAL},
    {"SANYO_AC", SANYO_AC}, {"VOLTAS", VOLTAS}, {"METZ", METZ},
    {"TRANSCOLD", TRANSCOLD}, {"TECHNIBEL_AC", TECHNIBEL_AC}, {"MIRAGE", MIRAGE},
    {"ELITESCREENS", ELITESCREENS}, {"PANASONIC_AC32", PANASONIC_AC32},
    {"MILESTAG2", MILESTAG2}, {"ECOCLIM", ECOCLIM}, {"XMP", XMP}, {"TRUMA", TRUMA},
    {"HAIER_AC176", HAIER_AC176}, {"TEKNOPOINT", TEKNOPOINT}, {"KELON", KELON},
    {"TROTEC_3550", TROTEC_3550}, {"SANYO_AC88", SANYO_AC88}, {"BOSE", BOSE},
    {"ARRIS", ARRIS}, {"RHOSS", RHOSS}, {"AIRTON", AIRTON}, {"COOLIX48", COOLIX48},
    {"HITACHI_AC264", HITACHI_AC264}, {"KELON168", KELON168}, {"HITACHI_AC296", HITACHI_AC296},
    {"DAIKIN200", DAIKIN200}, {"HAIER_AC160", HAIER_AC160}, {"CARRIER_AC128", CARRIER_AC128},
    {"TOTO", TOTO}, {"CLIMABUTLER", CLIMABUTLER}, {"TCL96AC", TCL96AC},
    {"BOSCH144", BOSCH144}, {"SANYO_AC152", SANYO_AC152}, {"DAIKIN312", DAIKIN312},
    {"GORENJE", GORENJE}, {"WOWWEE", WOWWEE}, {"CARRIER_AC84", CARRIER_AC84}, {"YORK", YORK}};

decode_type_t getProtocolEnumFromString(const String &proto)
{
    if (protocolMap.count(proto))
        return protocolMap[proto];
    return decode_type_t::UNKNOWN;
}

bool saveProtocolToFS(const String &name)
{
    File f = LittleFS.open("/protocol.txt", "w");
    if (!f)
        return false;
    f.println(name);
    f.close();
    return true;
}

String loadProtocolFromFS()
{
    if (!LittleFS.exists("/protocol.txt"))
        return "KELVINATOR";
    File f = LittleFS.open("/protocol.txt", "r");
    if (!f)
        return "KELVINATOR";
    String name = f.readStringUntil('\n');
    name.trim();
    f.close();
    return name.length() ? name : String("KELVINATOR");
}

bool updateProtocolFromString(const String &protocolName)
{
    decode_type_t protoEnum = getProtocolEnumFromString(protocolName);
    if (protoEnum != decode_type_t::UNKNOWN)
    {
        ac.next.protocol = protoEnum;
        lastProtocolName = protocolName;
        saveProtocolToFS(protocolName);
        Serial.println("更新协议为：" + protocolName);
        return true;
    }
    Serial.println("无法识别协议：" + protocolName);
    return false;
}

String getProtocolName()
{
    return lastProtocolName;
}

String handleCommand(const String &cmd)
{
    DBG("指令: %s\n", cmd.c_str());

    if (cmd == "power=off")
    {
        ac.next.power = false;
        irManager.send(ac.next.degrees, (int)ac.next.fanspeed, (int)ac.next.mode, false);
        return "power=off";
    }

    if (cmd.startsWith("temp="))
    {
        int idx1 = cmd.indexOf(';');
        int idx2 = cmd.indexOf(';', idx1 + 1);
        int idx3 = cmd.indexOf(';', idx2 + 1);
        if (idx1 < 0 || idx2 < 0)
            return "invalid";
        int temp = cmd.substring(5, idx1).toInt();
        int mode = cmd.substring(idx1 + 6, idx2).toInt();
        int speed = cmd.substring(idx2 + 7, idx3 < 0 ? cmd.length() : idx3).toInt();
        ac.next.power = true;
        irManager.send(temp, speed, mode, true);
        return String("temp=") + temp + ";mode=" + mode + ";speed=" + speed + ";power=on";
    }

    // 协议学习：10 秒内等待有效红外信号
    if (cmd == "learn=start")
    {
        irManager.beginLearn();
        unsigned long start = millis();
        while (millis() - start < 10000)
        {
            irManager.processReceive();
            String p = irManager.getLearnedProtocol();
            if (!p.isEmpty())
            {
                irManager.stopLearning();
                updateProtocolFromString(p);
                return "learn=success:" + p;
            }
            delay(50);
            yield();
        }
        irManager.stopLearning();
        return "learn=timeout";
    }

    if (cmd.startsWith("protocol="))
    {
        String proto = cmd.substring(9);
        bool ok = updateProtocolFromString(proto);
        return ok ? String("protocol=") + proto : "protocol=invalid";
    }

    if (cmd == "status")
    {
        char buffer[15];
        snprintf(buffer, sizeof(buffer), "t%.1fh%.1f", envTemperature, enHumidity);
        return String(buffer);
    }

    if (cmd == "power")
        return String("power=") + (ac.next.power ? "on" : "off");

    if (cmd == "get_protocol")
        return "protocol=" + lastProtocolName;

    if (cmd.startsWith("time="))
    {
        bool ok = timerManager.setTimeFromPhone(cmd.substring(5));
        return ok ? "time=ok" : "time=invalid";
    }

    if (cmd == "time")
        return "time=" + timerManager.getCurrentTime();

    if (cmd.startsWith("timer="))
    {
        String timerCmd = cmd.substring(6);
        if (timerCmd == "list")
            return "timers=" + timerManager.getTaskList();

        if (timerCmd.startsWith("add;"))
        {
            int hour = 0, minute = 0, temp = 25, mode = 0, speed = 0;
            bool power = true, repeat = false;
            int pos = 4;
            while (pos < timerCmd.length())
            {
                int semi = timerCmd.indexOf(';', pos);
                if (semi == -1) semi = timerCmd.length();
                String part = timerCmd.substring(pos, semi);
                if (part.startsWith("hour=")) hour = part.substring(5).toInt();
                else if (part.startsWith("minute=")) minute = part.substring(7).toInt();
                else if (part.startsWith("temp=")) temp = part.substring(5).toInt();
                else if (part.startsWith("mode=")) mode = part.substring(5).toInt();
                else if (part.startsWith("speed=")) speed = part.substring(6).toInt();
                else if (part.startsWith("power=")) power = (part.substring(6) == "on");
                else if (part.startsWith("repeat=")) repeat = (part.substring(7) == "1");
                pos = semi + 1;
            }
            int id = timerManager.addTask(hour, minute, temp, mode, speed, power, repeat);
            return (id >= 0) ? String("timer_add=success;id=") + id : "timer_add=failed";
        }

        if (timerCmd.startsWith("update;"))
        {
            int id = 0, hour = 0, minute = 0, temp = 25, mode = 0, speed = 0;
            bool power = true, repeat = false;
            int pos = 7;
            while (pos < timerCmd.length())
            {
                int semi = timerCmd.indexOf(';', pos);
                if (semi == -1) semi = timerCmd.length();
                String part = timerCmd.substring(pos, semi);
                if (part.startsWith("id=")) id = part.substring(3).toInt();
                else if (part.startsWith("hour=")) hour = part.substring(5).toInt();
                else if (part.startsWith("minute=")) minute = part.substring(7).toInt();
                else if (part.startsWith("temp=")) temp = part.substring(5).toInt();
                else if (part.startsWith("mode=")) mode = part.substring(5).toInt();
                else if (part.startsWith("speed=")) speed = part.substring(6).toInt();
                else if (part.startsWith("power=")) power = (part.substring(6) == "on");
                else if (part.startsWith("repeat=")) repeat = (part.substring(7) == "1");
                pos = semi + 1;
            }
            bool ok = timerManager.updateTask(id, hour, minute, temp, mode, speed, power, repeat);
            return ok ? String("timer_update=success;id=") + id : "timer_update=failed";
        }

        if (timerCmd.startsWith("delete;id="))
        {
            int id = timerCmd.substring(10).toInt();
            bool ok = timerManager.deleteTask(id);
            return ok ? String("timer_delete=success;id=") + id : "timer_delete=failed";
        }

        if (timerCmd.startsWith("enable;id="))
        {
            int idPos = timerCmd.indexOf(";id=") + 4;
            int statePos = timerCmd.indexOf(";state=");
            int id = timerCmd.substring(idPos, statePos).toInt();
            bool state = (timerCmd.substring(statePos + 7) == "1");
            bool ok = timerManager.enableTask(id, state);
            return ok ? String("timer_enable=success;id=") + id : "timer_enable=failed";
        }
    }

    if (cmd == "wifi_mode")
        return "already=wifi_mode";

    // 附加功能：强劲/扫风/面板灯/睡眠/自清洁（切换型）
    if (cmd == "turbo")
    {
        ac.next.turbo = !ac.next.turbo;
        irManager.send(ac.next.degrees, (int)ac.next.fanspeed, (int)ac.next.mode, ac.next.power);
        return String("turbo=") + (ac.next.turbo ? "on" : "off");
    }
    if (cmd == "swing")
    {
        ac.next.swingv = (ac.next.swingv == stdAc::swingv_t::kOff) ? stdAc::swingv_t::kAuto : stdAc::swingv_t::kOff;
        irManager.send(ac.next.degrees, (int)ac.next.fanspeed, (int)ac.next.mode, ac.next.power);
        return String("swing=") + (ac.next.swingv != stdAc::swingv_t::kOff ? "on" : "off");
    }
    if (cmd == "light")
    {
        ac.next.light = !ac.next.light;
        irManager.send(ac.next.degrees, (int)ac.next.fanspeed, (int)ac.next.mode, ac.next.power);
        return String("light=") + (ac.next.light ? "on" : "off");
    }
    if (cmd == "sleep")
    {
        ac.next.sleep = (ac.next.sleep < 0) ? 1 : -1;
        irManager.send(ac.next.degrees, (int)ac.next.fanspeed, (int)ac.next.mode, ac.next.power);
        return String("sleep=") + (ac.next.sleep >= 0 ? "on" : "off");
    }
    if (cmd == "clean")
    {
        ac.next.clean = !ac.next.clean;
        irManager.send(ac.next.degrees, (int)ac.next.fanspeed, (int)ac.next.mode, ac.next.power);
        return String("clean=") + (ac.next.clean ? "on" : "off");
    }

    // 协议学习：等待遥控器发射（10 秒超时）
    if (cmd == "learn=start")
    {
        irManager.beginLearn();
        unsigned long start = millis();
        String proto = "";
        while (proto.isEmpty() && millis() - start < 10000)
        {
            irManager.loop(); // 学习模式下捕获红外信号
            proto = irManager.getLearnedProtocol();
            delay(100);
        }
        irManager.stopLearning();
        if (!proto.isEmpty() && proto != "UNKNOWN")
        {
            updateProtocolFromString(proto);
            return String("learn=success:") + proto;
        }
        return "learn=timeout";
    }

    if (cmd == "reset")
    {
        ESP.restart();
        return "reset=ok";
    }

    // OTA：面板从 GitHub 查到最新版本后下发固定地址（手机端免手动设置）
    if (cmd.startsWith("ota=seturl "))
    {
        String u = cmd.substring(11);
        u.trim();
        return otaManager.setUrl(u) ? "ota=url ok" : "ota=url invalid";
    }

    // OTA（ESP8266 面板驱动版）：面板直接告诉设备最新版本号，设备自行比较
    if (cmd.startsWith("ota=check "))
    {
        String rest = cmd.substring(10);
        rest.trim();
        int sp = rest.indexOf(' ');
        String ver = (sp < 0) ? rest : rest.substring(0, sp);
        ver.trim();
        if (ver.length() > 0 && otaManager.isNewer(ver))
            return String("ota=found ") + ver;
        return String("ota=uptodate ") + otaManager.getVersion();
    }

    // OTA：开始异步下载（直接 HTTP 拉包）
    if (cmd == "ota=go")
    {
        if (otaManager.isDownloading())
            return "ota=busy";
        otaManager.requestDownload();
        return String("ota=start fw=") + otaManager.getVersion();
    }

    return "unknown_cmd:" + cmd;
}
