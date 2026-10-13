# main 目录功能说明

本目录为 DFR1266_VOREO 固件应用层：四麦采集、本地语音识别（ESP-SR）、与 GAT 模块/上位机通信、RGB 指示，以及可选的 DOA 测向。

## 文件一览

| 文件 | 职责 |
|------|------|
| `DFR1266_VOREO.c` | 入口 `app_main`，编排启动顺序 |
| `UserConfig.h` | 板级宏：Codec 型号、GPIO、UART/I2C、ASR 语言开关 |
| `UserCode.h` / `UserCode.c` | 麦克风业务、语音识别、DOA |
| `AudioInitialization.h` / `.c` | I2C + I2S + Codec（ES7210/ES7243E）驱动封装 |
| `PeripheralCode.h` / `.c` | RGB、GAT UART、用户 UART/I2C 从机、协议解析与数据链表 |
| `CMakeLists.txt` | 注册上述源文件 |
| `idf_component.yml` | 组件依赖声明 |

---

## 启动流程（当前默认）

```text
app_main()
  ├─ initMIC()              // 打开 ES7210 四麦采集
  ├─ peripheralInit()       // RGB / GAT UART / MODE 选 UART 或 I2C / userTask
  ├─ setRGBColor(RGB_BLACK)
  ├─ // autodetectSlotMapping()  // 调试用，默认关
  ├─ // initAudioDoa()           // DOA，默认关
  └─ initSpeechRecog()      // 加载 model 分区 → AFE → feed/detect 任务
```

---

## 模块说明

### 1. 入口 — `DFR1266_VOREO.c`

- 唯一公开入口：`app_main()`
- 决定哪些子系统在开机时启用（DOA / 槽位自检当前注释关闭）

### 2. 板级配置 — `UserConfig.h`

| 配置项 | 含义 |
|--------|------|
| `ES7210` / `ES7243E` | 二选一 Codec；当前用 ES7210（4 路 TDM） |
| `ESP_MN_CH_MODEL` / `ESP_MN_EN_MODEL` | MultiNet 中/英命令词 |
| `RGB_GPIO`、`MODE_GPIO` | 灯带、通信模式选择 |
| `GAT_*` | GAT 模组相关电源/唤醒/复位脚 |
| `USERUART_*` / `GTA_UART_*` | 用户侧与 GAT 侧串口引脚、波特率 |
| `USERI2C_ADDRESS` | I2C 从机地址 `0x1F` |

`MODE_GPIO` 电平决定用户侧走 **UART** 还是 **I2C 从机**（同一组 TX/SDA、RX/SCL 脚复用）。

### 3. 音频采集 — `AudioInitialization.*`

- `initAudio()`：创建 I2C 主控总线 + I2S（ES7210 为 TDM 四槽）+ `esp_codec_dev` 录音设备
- ES7210：仅 RX，无 TX；`i2s_channel_enable` 对空句柄做了保护
- 输出：`esp_codec_dev_handle_t`，供 `initMIC` / `feedTask` 读 PCM

### 4. 语音与 DOA — `UserCode.*`

#### 4.1 麦克风初始化 `initMIC()`

- 调用 `initAudio()`，配置采样率/通道增益，打开 Codec

#### 4.2 语音识别 `initSpeechRecog()`

1. `esp_srmodel_init("model")`：从 Flash `model` 分区加载 SR 模型  
2. `afe_config_init(...)`：ES7210 输入格式 `"MMNN"`（4 通道，前两路作麦）  
3. 关闭 WakeNet / AEC，优先从 PSRAM 分配  
4. 创建任务：  
   - **`feedTask`**（核 0）：读 Codec →（可选）送 DOA 队列 → 转 16bit → `afe->feed`  
   - **`detectTask`**（核 1）：`afe->fetch` + MultiNet 命令词检测  

> 未调用 `initAudioDoa()` 时 `audio_queue` 为 NULL，`feedTask` 跳过队列，只喂 AFE。

#### 4.3 DOA（可选）`initAudioDoa()` / `audioDoaTask`

- 创建 `audio_queue`，启动测向任务  
- 算法相关：GCC-PHAT / TDOA、GN 定位、Hann/FFT、卡尔曼角度平滑等（见 `DOA_4MIC_RECTANGULAR.md`）  
- 参数：`SAMPLE_RATE=16k`，`FRAME_SIZE=1024`，`CHANNELS=4`，麦阵边长约 50mm  

#### 4.4 其它

- `autodetectSlotMapping()`：敲击各麦，估计 TDM 槽位与物理麦映射（调试）  
- `setWakeWord()`：运行时改 MultiNet 命令词  

### 5. 外设与协议 — `PeripheralCode.*`

#### 5.1 `peripheralInit()`

| 步骤 | 内容 |
|------|------|
| RGB | `led_strip` 初始化 |
| GAT 链表 | ASR 文本等缓存 |
| GAT UART | UART1，115200，`gtaUartTask` 收 GAT 协议 |
| `userTask` | 周期性业务（如发角度/处理链表） |
| GPIO | MODE、GAT 电源/复位/唤醒等 |
| 用户通道 | MODE=I2C → I2C 从机 + `i2cSlaveTask` / `i2cDataProcessTask`；否则用户 UART + `userUartTask` |

#### 5.2 GAT 侧协议（UART1）

| 命令 | 值 | 含义 |
|------|-----|------|
| `GAT_CMD_WAKEUP` | `0xA0` | 唤醒 |
| `GAT_CMD_DETECTED` | `0xA1` | 检测 |
| `GAT_CMD_ASR_DATA` | `0xA2` | ASR 文本数据 |
| `GAT_CMD_OPEN` | `0xA3` | 开机 |
| `GAT_CMD_READ_ERROR` | `0xAA` | 读错误 |

帧格式含头 `0xFD 0x00`、命令、长度、数据、结束 `0xFF`；大包用链表节点拼包。

#### 5.3 用户侧协议（UART 或 I2C）

| 命令 | 值 | 含义 |
|------|-----|------|
| `USER_UART_CMD_BEGIN` | `0x00` | 握手 |
| `USER_UART_CMD_QUERY_TEXT` | `0x01` | 查询是否有文本 |
| `USER_UART_CMD_GET_TEXT` | `0x02` | 取 ASR 文本 |
| `USER_UART_CMD_SEND_TEXT` | `0x03` | 发文本 |
| `USER_UART_CMD_ANGLE` | `0x04` | 角度 |
| `USER_UART_CMD_DISTANCE` | `0x05` | 距离 |
| `USER_UART_CMD_SET_ASR` | `0x06` | 设置 ASR |
| `USER_UART_CMD_TTS_SPEED` | `0x07` | TTS 语速 |
| `USER_UART_CMD_GET_ASR_STATE` | `0x08` | ASR 状态 |

用户帧头 `0x55`，发送侧带 CRC16。I2C 从机另支持长度查询等（`I2C_SLAVE_CMD_*`），按长度/数据分包回传。

#### 5.4 RGB

- `setRGBColor(rgb_t)`：状态指示（黑/蓝/绿/黄等宏在头文件中定义）

---

## 任务一览

| 任务名 | 创建位置 | 作用 |
|--------|--------|------|
| `feed` | `initSpeechRecog` | 采麦 → 喂 AFE（可选推 DOA 队列） |
| `detect` | `initSpeechRecog` | AFE fetch + MultiNet |
| `gtaUartTask` | `peripheralInit` | 收 GAT 串口 |
| `userTask` | `peripheralInit` | 用户侧周期逻辑 |
| `userUartTask` | UART 模式 | 收用户串口并解析 |
| `i2cSlaveTask` | I2C 模式 | I2C 收发事件 |
| `i2cDataProcessTask` | I2C 模式 | 处理 I2C 收到的用户命令 |
| `audio_doa_task` | `initAudioDoa`（可选） | DOA 处理 |

---

## 模块连接关系

整体可看成：**一块 ESP32-S3 主控 + 三块外设域（麦阵 Codec / GAT / 用户口）+ 一套本地语音管线**。采麦与识别在 `UserCode` 内闭环；GAT 与用户侧经 `gatDataList` 交接文本。

### 硬件连接（板级）

```text
                    ┌──────────────────────────────────────┐
                    │              ESP32-S3                │
                    │                                      │
  4×Mic ──I2S TDM──►│ GPIO3/4/5/6  + I2C(10/11)            │◄── ES7210
                    │                                      │
  RGB LED ──RMT────►│ GPIO9                                │
                    │                                      │
  MODE 键 ─────────►│ GPIO21  ──决定用户口是 UART 还是 I2C──│
                    │                                      │
  GAT 模组 ◄─UART1─►│ TX38 / RX39 @115200                  │
                    │  电源/复位/唤醒: GPIO7/45/47/8…       │
                    │                                      │
  上位机/用户 ◄────►│ GPIO14/13                            │
                    │  UART2@9600  或  I2C从机@0x1F         │
                    └──────────────────────────────────────┘
```

| 外设 | 与 ESP 的接口 | 配置位置 |
|------|----------------|----------|
| ES7210 四麦 | I2S TDM + I2C 控制 | `AudioInitialization` / `UserConfig` |
| GAT（GTA）模组 | UART1 + 若干 GPIO | `PeripheralCode` / `UserConfig` |
| 用户侧 | UART2 **或** I2C 从机（`MODE_GPIO` 二选一） | `PeripheralCode` |
| RGB | GPIO9 | `PeripheralCode` |

### 软件调用关系

```text
DFR1266_VOREO.c (app_main)
        │
        ├──► UserCode.initMIC()
        │         └──► AudioInitialization.initAudio()
        │                   └── ES7210 / I2S / I2C master
        │
        ├──► PeripheralCode.peripheralInit()
        │         ├── RGB (led_strip)
        │         ├── GAT UART 任务  (gtaUartTask)
        │         ├── userTask
        │         └── 用户 UART 或 I2C 从机任务
        │
        └──► UserCode.initSpeechRecog()     // DOA 默认未开
                  ├── esp-sr model 分区
                  ├── AFE
                  ├── feedTask  ──读 Codec──► AFE.feed
                  └── detectTask ──AFE.fetch──► MultiNet
```

| 方向 | 说明 |
|------|------|
| 入口 | 只调 `UserCode` + `PeripheralCode` |
| UserCode | 依赖 `AudioInitialization`（采麦）和 esp-sr（识别） |
| PeripheralCode | 依赖 `UserConfig`；经链表/命令连 GAT、用户侧、可选 `angle` |
| AudioInitialization | 不依赖语音/协议，只提供 `record_dev` |

### 运行时数据流

```text
                    ┌──────────── 采麦域 ────────────┐
                    │  Mic → ES7210 → I2S → Codec   │
                    └───────────────┬───────────────┘
                                    │ PCM
                    ┌───────────────▼───────────────┐
                    │  UserCode: feed / detect      │
                    │  AFE + MultiNet（本地 SR）    │
                    └───────────────┬───────────────┘
                                    │（可选）audio_queue
                                    ▼
                              DOA → angle
                                    │
        ┌───────────────────────────┼───────────────────────────┐
        │                           │                           │
        ▼                           ▼                           ▼
┌───────────────┐         ┌─────────────────┐         ┌─────────────────┐
│ GAT UART      │         │ gatDataList     │         │ 用户 UART/I2C   │
│ 收：唤醒/ASR  │────────►│ 文本缓存链表     │◄────────│ 查/取文本、设ASR │
│ 发：用户文本  │◄────────│                 │         │ 读角度等         │
└───────────────┘         └─────────────────┘         └─────────────────┘
```

要点：

1. **采麦 → 语音**：只在 `UserCode` 内闭环（feed/detect），不经过 GAT。
2. **GAT → 链表**：GAT 的 ASR 文本（`0xA2`）进入 `gatDataList`。
3. **用户 ↔ 链表**：`GET_TEXT` 取 GAT 文本；`SEND_TEXT` 经 `userTask` 再 `sendBufToGAT` 发给模组。
4. **协议不同**：GAT 用 `FD 00 … FF`；用户用 `55 …` + CRC。
5. **RGB**：协议状态旁路指示（如唤醒绿、检测蓝），不参与数据通路。

### 谁连谁（总表）

| 模块 A | 连到 | 方式 | 作用 |
|--------|------|------|------|
| ES7210 | `AudioInitialization` | I2S/I2C | 四麦 PCM |
| `AudioInitialization` | `UserCode` | `record_dev` 句柄 | 读音频 |
| `UserCode` (AFE/MN) | Flash `model` | SPIFFS 分区 | 加载模型 |
| `UserCode` (feed) | `audio_queue` | 队列（可选） | 给 DOA |
| GAT 模组 | `PeripheralCode` | UART1 | 唤醒/ASR/文本互传 |
| 用户主机 | `PeripheralCode` | UART2 或 I2C | 查文本/控 ASR/读角度 |
| GAT ↔ 用户 | `gatDataList` | 内存链表 | 解耦两边协议与速率 |
| RGB | `PeripheralCode` | GPIO | 状态灯 |
| `MODE_GPIO` | `PeripheralCode` | 电平 | 切换用户口形态 |

一句话：**硬件上** ESP 居中接麦阵、GAT、用户口、RGB；**软件上** `UserCode` 管听与识别，`PeripheralCode` 管和 GAT/用户通信，两边靠 `gatDataList` 交接 ASR 文本。

---

## 启用可选功能

| 功能 | 改法 |
|------|------|
| DOA | `app_main` 取消注释 `initAudioDoa()`（建议放在 `initSpeechRecog` 之前） |
| 麦槽自检 | 取消注释 `autodetectSlotMapping()` |
| 换 Codec | `UserConfig.h` 改 `ES7210` / `ES7243E` |
| 用户改走 I2C | 硬件拉好 `MODE_GPIO` 对应电平 |

更细的 DOA 几何与算法说明见仓库根目录 `DOA_4MIC_RECTANGULAR.md`；固件合并烧录见 `README_MERGE_BIN.md`。
