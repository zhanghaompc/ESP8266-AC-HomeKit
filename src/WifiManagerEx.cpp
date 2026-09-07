#include "WifiManagerEx.h"
#include "LedManager.h"
#include "TimerManager.h"
#include "IrManager.h"
#include "DeviceConfig.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

extern LedManager ledManager;
extern TimerManager timerManager;
extern IrManager irManager;

static String jsonEscape(String value)
{
    value.replace("\\", "\\\\");
    value.replace("\"", "\\\"");
    value.replace("\n", "\\n");
    value.replace("\r", "\\r");
    return value;
}

WifiManagerEx::WifiManagerEx() : configServer(80) {}

void WifiManagerEx::begin()
{
    // 允许 WiFi 驱动在短暂丢包/漫游后自动恢复，减少设备长期离线。
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    // 关闭省电睡眠，减少部分路由器下的丢包、断线和恢复失败。
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    setupConfigPortalHandlers();
    // 注意：configServer.begin() 不能在 begin() 里调用——此刻 WiFi 尚未
    // 就绪，创建监听 socket 可能出问题。它在 startConfigPortal() 里
    // WiFi 就绪后才 begin。
}

void WifiManagerEx::enable()
{
    radioEnabled = true;
    connectWiFi();
}

void WifiManagerEx::disable()
{
    radioEnabled = false;
    stopConfigPortal();
}

void WifiManagerEx::loop()
{
    if (!radioEnabled)
        return;

    if (configPortalActive)
    {
        dnsServer.processNextRequest();
        configServer.handleClient();
    }

    checkWiFiConnection();
}

bool WifiManagerEx::isConnected() const
{
    return wifiConnected;
}

// STA 重连退避：5s -> 10s -> 20s -> 30s 封顶
unsigned long WifiManagerEx::currentBackoff() const
{
    switch (retryCount)
    {
    case 0:
        return 0;
    case 1:
        return 5000;
    case 2:
        return 10000;
    case 3:
        return 20000;
    default:
        return 30000;
    }
}

void WifiManagerEx::beginStaConnect()
{
    String ssid, pass;
    hasCredentials = loadWifiCredentials(ssid, pass);

    // 模式固定 AP_STA：AP 是否广播由 softAP/softAPdisconnect 控制，
    // 不再用 WiFi.mode() 去切换，避免把 STA 一起关掉。
    if (WiFi.getMode() != WIFI_AP_STA)
        WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);

    lastStaAttemptTime = millis();
    if (retryCount < 255)
        retryCount++;

    if (hasCredentials)
    {
        Serial.printf("[WiFi] 尝试连接 %s (第 %u 次)\n", ssid.c_str(), retryCount);
        WiFi.begin(ssid.c_str(), pass.c_str());
    }
    else if (pendingSsid.length() > 0)
    {
        Serial.printf("[WiFi] 尝试连接配网页提交的 %s\n", pendingSsid.c_str());
        WiFi.begin(pendingSsid.c_str(), pendingPass.c_str());
    }
    else
    {
        // 兼容旧固件：凭据可能只在 SDK 存储里，WiFi.begin() 无参能自动读取
        Serial.println("[WiFi] 无 /wifi.json 凭据，尝试 SDK 中的旧凭据");
        WiFi.begin();
    }
}

void WifiManagerEx::connectWiFi()
{
    String ssid, pass;
    hasCredentials = loadWifiCredentials(ssid, pass);

    retryCount = 0;
    staDownSince = millis();
    ledManager.blinkGreen();

    if (!hasCredentials)
    {
        // 完全没配过：直接开热点等配网，不用白等重连超时
        Serial.println("[WiFi] 未配置 WiFi，直接开启配网热点");
        startConfigPortal();
        return;
    }

    beginStaConnect();
}

void WifiManagerEx::checkWiFiConnection()
{
    unsigned long now = millis();

    // 扫描期间临时暂停 STA：此时射频要让给扫描，不触发重连/退避/开热点
    if (staPausedForScan)
        return;

    bool linkUp = (WiFi.status() == WL_CONNECTED);

    if (linkUp)
    {
        if (!wifiConnected)
        {
            wifiConnected = true;
            retryCount = 0;
            staDownSince = 0;
            if (!irManager.isLearning())
            {
                ledManager.stopBlink();
                ledManager.off(); // WiFi 已连接 = 熄灭
            }
            Serial.printf("[WiFi] 已连接！IP: %s\n", WiFi.localIP().toString().c_str());
            timerManager.syncTime();

            // 连上就关掉配网热点：AP_STA 长期共存会拖累 STA，
            // 也不该在已联网时还多广播一个 SSID。
            if (configPortalActive)
            {
                // 留一点时间把 /save 的响应发回手机，再关热点
                if (apLingerMs)
                    delay(apLingerMs);
                stopConfigPortal();
            }
        }
        return;
    }

    // ---- 未连接 ----
    if (wifiConnected)
    {
        wifiConnected = false;
        retryCount = 0;
        staDownSince = now;
        Serial.println("[WiFi] 连接已断开，开始退避重连");
    }
    if (staDownSince == 0)
        staDownSince = now;

    // 学习红外协议期间不覆盖紫色常亮
    if (!irManager.isLearning())
        ledManager.blinkGreen(); // 同色去重，不会重置闪烁计时

    // 断连超过宽限期才开热点。这段时间先安静重连，
    // 避免每次短暂丢包都弹一个热点出来。
    if (!configPortalActive && (now - staDownSince >= staGraceMs || !hasCredentials))
    {
        Serial.printf("[WiFi] 断连已超过 %lu 秒，开启配网热点\n", staGraceMs / 1000);
        startConfigPortal();
    }

    // 无论热点是否开着，STA 都按退避节奏继续重试。
    // 门户开启期间降频到 60s 一次：反复 WiFi.begin() 会抢占射频，
    // 干扰配网页的 DHCP/HTTP 响应，导致手机打不开配置页。
    unsigned long backoff = configPortalActive ? 60000UL : currentBackoff();
    if (now - lastStaAttemptTime >= backoff)
        beginStaConnect();
}

void WifiManagerEx::startAccessPoint()
{
    String apName = deviceApName(); // 例如 ESP8266AC_a1b2

    // 只在 AP_STA 下开热点，绝不切成纯 WIFI_AP —— 那样会关掉 STA，
    // 设备就再也没机会自己连回去了。
    if (WiFi.getMode() != WIFI_AP_STA)
        WiFi.mode(WIFI_AP_STA);

    // 显式重建 AP 的 IP/网关/掩码与 DHCP 服务配置，避免复用残留的
    // 脏状态（这是"复位后能开、反复开关后打不开"的常见原因之一）。
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));

    // 开放热点（无密码）。channel 跟随 STA 会更稳，但未连接时用 1。
    bool ok = WiFi.softAP(apName.c_str());
    Serial.printf("[AP] softAP(%s) result=%d mode=%d ip=%s\n",
                  apName.c_str(), ok ? 1 : 0, (int)WiFi.getMode(),
                  WiFi.softAPIP().toString().c_str());
}

// 重置 WiFi 栈：停掉 AP 与 STA → 关驱动 → 以 AP_STA 重新初始化。
void WifiManagerEx::resetWifiStack()
{
    Serial.println("[WiFi] WiFi 栈已重置，重新初始化 AP_STA");
    WiFi.setAutoReconnect(false);
    WiFi.softAPdisconnect(true); // 先彻底停掉 AP 接口（含 DHCP server）
    WiFi.disconnect(true);       // 断开 STA 并停止射频
    delay(200);
    WiFi.mode(WIFI_OFF);         // 关闭 WiFi 驱动
    delay(500);
    WiFi.mode(WIFI_AP_STA);      // 重新以 AP_STA 初始化
    WiFi.setAutoReconnect(true);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    staPausedForScan = false;
}

void WifiManagerEx::startConfigPortal()
{
    if (configPortalActive)
        return;

    // 手动触发（按键/指令）时把宽限计时归零，避免刚开就被别处逻辑判定该关
    if (staDownSince == 0)
        staDownSince = millis();

    // 清理 WiFi 栈：AP_STA 反复重连/开关热点后，驱动可能残留 STA connecting
    // 状态，导致配网页打不开或扫描异常。先彻底重置再拉起热点。
    resetWifiStack();

    startAccessPoint();

    // captive portal：把所有域名解析到 AP IP，手机自动弹出配网页
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());
    // 配网 HTTP 服务器：此刻 WiFi 已 AP_STA 就绪，可以安全创建监听 socket。
    // 端口用 80：8266 的 HomeKit HAP 实际监听 5556，80 空闲，
    // 手机连上热点访问 http://192.168.4.1 即弹出配网页。
    configServer.begin(80);

    configPortalActive = true;
    ledManager.blinkGreen();
    Serial.printf("[AP] 配网门户已开启，SSID=%s 配网页=http://%s\n",
                  deviceApName().c_str(), WiFi.softAPIP().toString().c_str());
}

void WifiManagerEx::stopConfigPortal()
{
    if (!configPortalActive)
        return;

    configServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    // 保持 AP_STA 模式但不广播：下次要配网时 softAP() 直接拉起即可
    configPortalActive = false;
    scanState = -2;
    Serial.println("[AP] 配网门户已关闭，热点停止广播");
}

void WifiManagerEx::setupConfigPortalHandlers()
{
    if (configHandlersReady)
        return;

    configServer.on("/", HTTP_GET, [this]() {
        sendConfigPage();
    });

    configServer.on("/status", HTTP_GET, [this]() {
        String json = "{\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
                      ",\"portal\":" + String(configPortalActive ? "true" : "false") +
                      ",\"apip\":\"" + jsonEscape(WiFi.softAPIP().toString()) +
                      "\",\"ip\":\"" + jsonEscape(WiFi.localIP().toString()) +
                      "\",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"}";
        configServer.send(200, "application/json; charset=utf-8", json);
    });

    configServer.on("/scan", HTTP_GET, [this]() { handleScanRequest(); });

    configServer.on("/save", HTTP_POST, [this]() {
        String ssid = configServer.arg("ssid");
        String pass = configServer.arg("pass");
        ssid.trim();
        if (ssid.length() == 0)
        {
            configServer.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"message\":\"请先选择要连接的 WiFi\"}");
            return;
        }

        saveWifiCredentials(ssid, pass);

        pendingSsid = ssid;
        pendingPass = pass;
        hasCredentials = true;
        retryCount = 0;
        staDownSince = millis();
        staPausedForScan = false; // 若扫描期间暂停了 STA，先恢复，避免状态冲突

        // 先把响应发回手机，再发起连接，避免连接抖动把 HTTP 响应吞掉
        String message = "{\"ok\":true,\"message\":\"已保存，正在连接 " + jsonEscape(ssid) + "\"}";
        configServer.send(200, "application/json; charset=utf-8", message);

        WiFi.setAutoReconnect(true);
        WiFi.setSleepMode(WIFI_NONE_SLEEP);
        beginStaConnect();
    });

    configServer.onNotFound([this]() {
        configServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
        configServer.send(302, "text/plain", "");
    });

    configHandlersReady = true;
}

// 异步扫描：不阻塞主循环，AP 广播和 DNS 继续响应，手机不会掉线。
// 前端拿到 scanning=true 就轮询重试。
void WifiManagerEx::handleScanRequest()
{
    int done = WiFi.scanComplete();

    // 尚未发起，或上次结果已被取走：启动一次新扫描
    if (scanState == -2 && done != WIFI_SCAN_RUNNING)
    {
        // ESP8266 只有一根 2.4GHz 射频。STA 处于"连接中/重连中"时驱动会拒绝扫描
        // （日志：sta_scan: STA is connecting, scan are not allowed!），导致配网页
        // 扫不到任何 WiFi。先临时断开 STA，扫描完成后再恢复。
        WiFi.setAutoReconnect(false); // 先关自动重连再断开，避免驱动抢着重连
        WiFi.disconnect(false);       // 只断开连接：不清 SDK 凭据、不关射频
        staPausedForScan = true;

        WiFi.scanDelete();
        // async=true，隐藏 SSID 也一并返回
        WiFi.scanNetworks(true, true);
        scanState = -1;
        scanStartTime = millis();
        configServer.send(200, "application/json; charset=utf-8",
                          "{\"ok\":true,\"scanning\":true,\"count\":0,\"items\":[],"
                          "\"message\":\"正在扫描附近 2.4GHz WiFi…\"}");
        return;
    }

    if (done == WIFI_SCAN_RUNNING)
    {
        if (millis() - scanStartTime > scanTimeoutMs)
        {
            WiFi.scanDelete();
            scanState = -2;
            resumeStaAfterScan();
            configServer.send(200, "application/json; charset=utf-8",
                              "{\"ok\":false,\"scanning\":false,\"count\":0,\"items\":[],"
                              "\"message\":\"扫描超时，请重试或手动输入名称\"}");
            return;
        }
        configServer.send(200, "application/json; charset=utf-8",
                          "{\"ok\":true,\"scanning\":true,\"count\":0,\"items\":[],"
                          "\"message\":\"扫描中…\"}");
        return;
    }

    if (done < 0)
    {
        WiFi.scanDelete();
        scanState = -2;
        resumeStaAfterScan();
        configServer.send(200, "application/json; charset=utf-8",
                          "{\"ok\":false,\"scanning\":false,\"count\":0,\"items\":[],"
                          "\"message\":\"扫描失败，请重试或手动输入名称\"}");
        return;
    }

    Serial.printf("[AP] 扫描完成: %d 个 (mode=%d)\n", done, (int)WiFi.getMode());

    String json = "{\"ok\":true,\"scanning\":false,\"count\":" + String(done) + ",\"items\":[";
    int visible = 0;
    for (int i = 0; i < done; i++)
    {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0)
            continue;
        if (visible++)
            json += ",";
        json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                ",\"open\":" + String(WiFi.encryptionType(i) == ENC_TYPE_NONE ? "true" : "false") + "}";
    }
    json += "],\"message\":\"" +
            String(visible ? "扫描完成" : "没有发现可见的 2.4GHz WiFi") + "\"}";

    WiFi.scanDelete();
    scanState = -2; // 允许下次重新扫描
    resumeStaAfterScan();
    configServer.send(200, "application/json; charset=utf-8", json);
}

// 扫描结束（成功/失败/超时）后恢复 STA 重连：清暂停标记并重新发起连接
void WifiManagerEx::resumeStaAfterScan()
{
    if (!staPausedForScan)
        return;
    staPausedForScan = false;
    beginStaConnect(); // 内部会重设 autoReconnect/sleep，并重连当前凭据
}

bool WifiManagerEx::loadWifiCredentials(String &ssid, String &pass)
{
    if (!LittleFS.exists("/wifi.json"))
        return false;
    File f = LittleFS.open("/wifi.json", "r");
    if (!f)
        return false;
    JsonDocument doc;
    bool ok = false;
    if (deserializeJson(doc, f) == DeserializationError::Ok)
    {
        ssid = doc["ssid"] | "";
        pass = doc["pass"] | "";
        ok = ssid.length() > 0;
    }
    f.close();
    return ok;
}

void WifiManagerEx::saveWifiCredentials(const String &ssid, const String &pass)
{
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    File f = LittleFS.open("/wifi.json", "w");
    if (f)
    {
        serializeJson(doc, f);
        f.close();
        Serial.printf("WiFi 凭据已保存: %s\n", ssid.c_str());
    }
    else
    {
        Serial.println("保存 WiFi 凭据失败");
    }
}

// 配网页流式发送：HTML 全部放 flash(PROGMEM)，分块 sendContent_P 输出，
// 不在堆里构建大 String。AP 模式下内存紧张，一次性大分配会挤爆 WiFi 收包栈
// （曾导致 Exception 29 崩溃：buildConfigPageHtml -> String reserve -> WiFi 栈野指针）。
// 配网页流式发送：HTML 全部放 flash(PROGMEM)，分块 sendContent_P 输出，
// 不在堆里构建大 String。AP 模式下内存紧张，一次性大分配会挤爆 WiFi 收包栈
// （曾导致 Exception 29 崩溃：buildConfigPageHtml -> String reserve -> WiFi 栈野指针）。
void WifiManagerEx::sendConfigPage()
{
    // chunked 编码：必须设 CONTENT_LENGTH_UNKNOWN，否则 send("") 会带 Content-Length: 0，
    // 后续 sendContent_P 追加的 body 全部被浏览器丢弃（白屏）
    configServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    configServer.send(200, "text/html; charset=utf-8", "");

    static const char P1[] PROGMEM =
        "<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP8266AC 配网</title><style>"
        ":root{--text:#1c1e21;--muted:#6b7280;--line:#e5e7eb;--blue:#0a84ff;--green:#28a745;--red:#d64545}"
        "*{box-sizing:border-box}body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei',sans-serif;background:#f5f6f8;color:var(--text);-webkit-font-smoothing:antialiased}"
        ".wrap{max-width:400px;margin:0 auto;padding:36px 16px 24px}"
        ".card{background:#fff;border:1px solid var(--line);border-radius:16px;padding:20px}"
        ".header{display:flex;justify-content:space-between;align-items:center;gap:12px}"
        ".title{font-size:21px;font-weight:700;margin:0}"
        ".label-row{display:flex;justify-content:space-between;align-items:baseline;margin:16px 0 6px}"
        ".label-row label{margin:0}"
        ".hint{font-size:12px;color:var(--muted)}"
        ".status{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--muted)}"
        ".dot{width:8px;height:8px;border-radius:50%;flex:none;background:var(--blue)}.dot.green{background:var(--green)}"
        ".meta{display:grid;gap:8px;margin-top:14px;padding:12px 14px;background:#f8f9fa;border:1px solid var(--line);border-radius:12px;font-size:13px}"
        ".meta-row{display:flex;justify-content:space-between;gap:12px;color:var(--muted)}"
        ".meta-row b{color:var(--text);font-weight:600;word-break:break-all;text-align:right}"
        "label{display:block;font-size:13px;font-weight:600;margin:16px 0 6px}"
        "input{width:100%;padding:12px 14px;border:1px solid var(--line);border-radius:10px;font-size:15px;outline:none;background:#fff;color:var(--text)}"
        "input:focus{border-color:var(--blue);box-shadow:0 0 0 3px rgba(10,132,255,.12)}"
        "#scanBtn{width:100%;margin-top:10px;padding:11px;border:1px solid var(--line);border-radius:10px;background:#fafbfc;font-size:14px;color:var(--text);cursor:pointer}";
    configServer.sendContent_P(P1, sizeof(P1) - 1);

    static const char P2[] PROGMEM =
        "#scanBtn:disabled{opacity:.55;cursor:not-allowed}"
        ".list{display:grid;gap:6px;margin-top:10px;max-height:220px;overflow:auto}"
        ".item{display:flex;justify-content:space-between;align-items:center;gap:8px;width:100%;padding:11px 12px;border:1px solid var(--line);border-radius:10px;background:#fff;font-size:14px;text-align:left;cursor:pointer}"
        ".item small{color:var(--muted);font-size:12px;white-space:nowrap}"
        "#saveBtn{width:100%;margin-top:16px;padding:13px;border:0;border-radius:10px;background:var(--blue);color:#fff;font-size:15px;font-weight:600;cursor:pointer}"
        ".msg{margin-top:10px;min-height:18px;font-size:13px;line-height:1.5}"
        ".msg.ok{color:var(--green)}.msg.err{color:var(--red)}.msg.info{color:var(--blue)}"
        ".foot{margin-top:14px;font-size:12px;color:var(--muted);line-height:1.6}"
        "</style></head><body><div class='wrap'>"
        "<div class='card'>"
        "<div class='header'><div class='title'>ESP8266AC 配网</div><div class='status'><span class='dot' id='dot'></span><span id='statusText'>等待配置</span></div></div>"
        "<div class='meta'><div class='meta-row'><span>设备编号</span><b>";
    configServer.sendContent_P(P2, sizeof(P2) - 1);
    configServer.sendContent(deviceApName().c_str());

    static const char P3[] PROGMEM =
        "</b></div><div class='meta-row'><span>MQTT 主题</span><b>";
    configServer.sendContent_P(P3, sizeof(P3) - 1);
    configServer.sendContent(deviceMqttBase().c_str());

    static const char P4[] PROGMEM =
        "</b></div></div>"
        "<form id='wifiForm'>"
        "<div class='label-row'><label for='ssid'>WiFi 名称</label><span class='hint'>仅支持 2.4GHz WiFi</span></div>"
        "<input id='ssid' name='ssid' list='wifiOptions' placeholder='扫描选择，或手动输入' autocomplete='off'>"
        "<datalist id='wifiOptions'></datalist>"
        "<button type='button' id='scanBtn' onclick='doScan()'>扫描附近 WiFi</button>"
        "<div id='scanList' class='list'></div>"
        "<label for='pass'>WiFi 密码</label>"
        "<input type='password' name='pass' id='pass' placeholder='开放网络可留空' autocomplete='off'>"
        "<button type='submit' id='saveBtn'>保存并连接</button>"
        "</form>"
        "<div id='msg' class='msg'></div>"
        "</div>"
        "<div class='foot'>配网页地址 http://192.168.4.1（连上热点后手机一般会自动弹出，没弹就手动输入）</div>"
        "<script>"
        "function $(id){return document.getElementById(id);}"
        "var msg=$('msg'),scanBtn=$('scanBtn'),scanList=$('scanList'),ssid=$('ssid'),pass=$('pass');"
        "function setMsg(t,k){msg.className='msg '+(k||'');msg.textContent=t||'';}"
        "function refreshStatus(){fetch('/status').then(function(r){return r.json()}).then(function(s){var dot=$('dot'),st=$('statusText');if(s.connected){dot.className='dot green';st.textContent=s.ssid?s.ssid+' 已连接':'WiFi 已连接';}else{dot.className='dot';st.textContent='等待配置';}}).catch(function(){});}";
    configServer.sendContent_P(P4, sizeof(P4) - 1);

    static const char P5[] PROGMEM =
        "function renderScan(data){var list=data.items||[];var options=$('wifiOptions');options.innerHTML='';scanList.innerHTML='';scanBtn.disabled=false;scanBtn.textContent='重新扫描';if(!list.length){setMsg(data.message||'没有扫到可用 WiFi，可手动输入名称','info');return;}list.forEach(function(w){var o=document.createElement('option');o.value=w.ssid;options.appendChild(o);var row=document.createElement('button');row.type='button';row.className='item';row.onclick=function(){ssid.value=w.ssid;setMsg('已选择 '+w.ssid,'ok');};row.innerHTML='<span>'+w.ssid+'</span><small>'+(w.open?'开放':'加密')+' · '+w.rssi+'dBm</small>';scanList.appendChild(row);});}"
        "var scanTries=0;"
        "function pollScan(){fetch('/scan').then(function(r){return r.json()}).then(function(data){"
        "if(data.scanning&&scanTries++<20){setMsg('扫描中…','info');setTimeout(pollScan,1000);return;}"
        "scanTries=0;renderScan(data);"
        "}).catch(function(){scanTries=0;scanBtn.disabled=false;setMsg('扫描失败，可手动输入 WiFi 名称','err');});}"
        "function doScan(){scanBtn.disabled=true;scanTries=0;scanList.innerHTML='';setMsg('正在扫描附近 2.4GHz WiFi…','info');pollScan();}";
    configServer.sendContent_P(P5, sizeof(P5) - 1);

    static const char P6[] PROGMEM =
        "$('wifiForm').addEventListener('submit',function(e){e.preventDefault();var s=ssid.value.trim();if(!s){setMsg('请先选择或输入 WiFi 名称','err');return;}setMsg('正在保存并连接 '+s+'…','info');var f=new FormData();f.append('ssid',s);f.append('pass',pass.value);fetch('/save',{method:'POST',body:f}).then(function(r){return r.json()}).then(function(j){setMsg(j.message||'已保存，正在连接','ok');refreshStatus();}).catch(function(){setMsg('保存失败，请重试','err');});});"
        "refreshStatus();setInterval(refreshStatus,3000);"
        "</script></div></body></html>";
    configServer.sendContent_P(P6, sizeof(P6) - 1);
}
