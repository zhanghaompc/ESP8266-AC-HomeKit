# ESP8266 OTA 测试流程

## 本次版本

- 测试版本：`1.0.10`
- OTA 清单：
  `https://fastly.jsdelivr.net/gh/zhanghaompc/ESP8266-AC-HomeKit@808c3260de698f5885a60d013bca6f0764f16b86/firmware/esp8266/ota.json`
- 固件下载：
  `https://fastly.jsdelivr.net/gh/zhanghaompc/ESP8266-AC-HomeKit@6be8a53081235cdbfd5271b8deea399ae065614e/firmware/esp8266/esp8266_wifi.bin`

## 前置条件

1. 设备当前至少已经刷入 `1.0.8`。
2. 网页控制台设备类型选择 `ESP8266 空调`。
3. MQTT 主题填写设备真实主题，例如 `ac/esp8266ac7f63`。
4. 网页 MQTT 状态为已连接，设备状态为在线。

## 测试步骤

1. 打开控制台：
   `D:\ESP-Project\15.ESP32_AC_USB _ble\15.ESP32_AC_USB _ble\MQTT_CONTROL\index.html`
2. 点击“检查更新”。
3. 正常应提示发现新版本 `v1.0.10`。
4. 点击确认更新。
5. 设备收到 `ota=go` 后会短暂断开 MQTT，这是正常现象，目的是释放 ESP8266 内存。
6. 等待固件下载、写入、自动重启。
7. 设备重新上线后，网页固件版本应显示 `1.0.10`。
8. 再点一次“检查更新”，应显示“已是最新版本 v1.0.10”。

## 异常判断

- 如果显示 `HTTP GET -1 heap=xxxx`：
  记录完整日志和 heap 数值，说明还是下载前内存不足或 HTTPS 连接失败。
- 如果显示读取升级清单失败：
  先在浏览器打开 OTA 清单地址，确认能看到 `version` 和 `url`。
- 如果网页一直显示下载中：
  等设备重新上线后再点一次“检查更新”，看固件版本是否已经变成 `1.0.10`。

## 发布新版本时的固定流程

1. 修改 `src/DeviceConfig.h` 里的 `FW_VERSION`。
2. 编译：
   `C:\Users\zhanghao\.platformio\penv\Scripts\platformio.exe run -e esp8266_homekit`
3. 复制固件：
   `.pio\build\esp8266_homekit\firmware.bin` 到 `firmware\esp8266\esp8266_wifi.bin`
4. 提交并发布固件到 GitHub。
5. 用固件所在 commit 的固定地址更新 `firmware\esp8266\ota.json`。
6. 再提交并发布 OTA 清单。
7. 更新网页控制台中的 ESP8266 清单地址到新的清单 commit。
