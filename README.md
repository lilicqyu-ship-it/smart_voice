# smart_voice — AI 语音交互助手

基于 **ESP32-S3-LCD-EV-Board**（主板 v1.5 + SUB3 屏）的离线唤醒 + 本地固定指令语音控制系统：

```
 说「你好小智」 ──► 聆听 ──► ESP-SR MultiNet 本地指令识别 ──► 本地 TTS ──► 喇叭播放
      ▲                                                                            │
      └────────────────────── 点按屏幕 / BOOT 键可打断并重新说话 ◄──────────────────┘
```

## 功能

- **离线唤醒词**：esp-sr WakeNet9，「你好小智」（在 menuconfig 中可换「Hi,ESP」「你好小鑫」等）
- **离线语音控制**：MultiNet6 本地识别 RGB 灯和 CJDH11B 固定指令，不依赖网络
- **离线语音播报**：ESP-SR 中文 TTS 直接通过喇叭播报确认
- **云端对话**：关闭离线模式后仍可使用 OpenAI 兼容接口进行 ASR/LLM/TTS
- **屏幕 UI**：LVGL 9 中文界面（状态动画 + 对话气泡），GT1151 电容触摸
- **打断**：播放/思考中点按屏幕或按 BOOT 键，立即停止并进入聆听
- **状态灯**：板载 WS2812 呼吸灯跟随状态（绿=聆听，蓝闪=思考，青=播报，红=错误）

## 硬件对应

| 功能 | 芯片 | 引脚/地址 |
|---|---|---|
| 显示 | ST7262E43 RGB 800×480 | DE=17, VSYNC=3, HSYNC=46, PCLK=9, D0~D15=10/11/12/13/14/21/8/18/45/38/39/40/41/42/2/1 |
| 触摸 | GT1151 (I2C) | SDA=47, SCL=48（v1.5；v1.4 为 8/18，BSP 按模组自动探测） |
| 喇叭 | ES8311 + NS4150 | I2S: MCLK=5, BCLK=16, WS=7, DOUT=6；功放=TCA9554 P0 |
| 麦克风 | ES7210 双麦 | I2S DIN=15；I2C 地址 0x82（8 位） |
| 外接 RGB 灯 | TCA9554 | P1=G，P2=R，P3=B |
| CJDH11B 控制 | TCA9554 | P4 / EX_IO4，高电平打开、低电平关闭 |
| 状态灯 | WS2812 | GPIO4 |
| 按键 | BOOT | GPIO0 |

> 音频说明：TX/RX 共享同一组 BCLK/WS，因此固定 16 kHz / 立体声 / 16-bit。
> AEC（回声消除）参考信号需要 ES7210 TDM 多通道采集，本固件暂未启用；
> 打断交互通过触屏实现。引脚与时序取自 [esp-bsp/esp32_s3_lcd_ev_board](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_s3_lcd_ev_board)。

## 编译烧录

需要 ESP-IDF（≥5.3，本机使用 v6.1，`~/.espressif/tools/activate_idf_v6.1.sh`）。

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py set-target esp32s3

# 配置 WiFi 与 AI 服务（也可以直接编辑 sdkconfig）
idf.py menuconfig
#   Smart Voice Configuration -> WiFi Station / AI Service

idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor   # flash 会同时烧录唤醒词模型分区
```

首次构建会自动下载 esp-bsp / esp-sr / esp_codec_dev 等组件和唤醒词模型。

## 配置 AI 服务

默认对接 [SiliconFlow](https://siliconflow.cn)（一个 Key 同时提供 ASR/LLM/TTS）：

1. 注册后创建 API Key；
2. `idf.py menuconfig` → *Smart Voice Configuration → AI Service*：
   - `API base URL`：`https://api.siliconflow.cn/v1`
   - `API Key`：填入你的 Key
   - 可替换 LLM（如 `Qwen/Qwen3-8B`）、TTS 音色（`...:anna`/`:alex`/`:bella` 等）
3. 任何 OpenAI 兼容服务均可：把 base URL / 模型名换成对应值即可（TTS 接口需支持
   `POST /audio/speech` 返回 WAV；采样率不限，板端自动重采样）。

## 使用

1. 上电等待离线语音模型初始化完成；
2. 说「**你好小智**」，听到提示音后说「打开红灯」「打开蓝灯」「关闭灯」「打开设备」等固定指令；
3. 助手回答会逐句播报；期间**点按屏幕**可打断并直接再次说话；
4. 没有唤醒词时点按屏幕（或 BOOT 键）也可以直接开启对话。

## 常见问题

| 现象 | 处理 |
|---|---|
| 屏幕不亮/花屏 | 确认使用的是 SUB3（4.3" 800×480）屏；SUB2 (480×480) 需在 menuconfig 把 BSP 子板改为 480×480 |
| 屏幕闪烁/横线撕裂 | 已按实测验证的方案配置：`LVGL_DIRECT_MODE` + `AVOID_TEAR` + 3 块帧缓冲（LVGL 只重绘脏区到 vsync 同步的缓冲）+ bounce 缓冲 20 行 + `SPIRAM_XIP_FROM_PSRAM`（flash 写期间 cache 不再被关闭，否则 RGB bounce 中断读不到帧缓冲会雪花）+ 64KB/64B 行数据缓存。若仍偶发，确认供电 ≥1A |
| 触摸偏移 | GT1151/GT911 分辨率与屏幕一致，检查是否为第三方 800×400 屏（需自行改 BSP 时序 `SUB_BOARD3_800_480_PANEL_35HZ_RGB_TIMING`） |
| 唤醒不灵敏 | 对着板载双麦克风说话（主板下沿两个小孔），距离 1~3 m；可在 menuconfig 更换唤醒词 |
| 没有「model」分区报错 | 先 `idf.py flash`（模型分区随固件一起烧录），不要只烧 app |
| I2C 报错 | v1.4 主板与 v1.5 引脚不同，esp-bsp 按 PSRAM 大小自动选择；异常时用 8MB 模组+v1.4 板请改用 v1.4 引脚（SDA=8/SCL=18） |
| 语音识别为空 | 靠近说话或增大 *Microphone gain*（默认 30 dB） |
| 中文显示方框 | 已内置全量字库 `main/fonts/font_cjk_20.c`（GB2312 全部 6763 汉字 + ASCII + 常用标点，思源黑体 20px）。若个别生僻字仍显示为方框，用 `tools/gen_font.sh` 追加字符重新生成 |

## 代码结构

```
main/
├── main.c          应用状态机（IDLE/LISTENING/THINKING/SPEAKING）
├── board.c         WS2812 状态灯、BOOT 按键、功放控制（TCA9554）
├── board_audio.c   I2S 双工 + ES8311 播放 + ES7210 双麦录音
├── app_afe.c       esp-sr AFE：喂音/检出双任务，唤醒词 + VAD + 本地命令识别
├── app_local.c     MultiNet6 本地固定命令 + 中文 TTS
├── app_wifi.c      WiFi STA + SNTP
├── app_ai.c        ASR(multipart) / LLM(SSE 流式+分句) / TTS(WAV 下载+重采样)
├── app_ui.c        LVGL 界面（状态动画、对话气泡、触屏打断）
├── fonts/          生成的中文字库（font_cjk_20，见 tools/gen_font.sh）
├── app_priv.h      公共状态与事件
└── Kconfig.projbuild  WiFi / AI 服务 / 音频参数配置
```
