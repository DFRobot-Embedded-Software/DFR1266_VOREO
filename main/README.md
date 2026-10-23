# main 目录功能说明

本目录为 DFR1266_VOREO 固件应用层：四麦采集、声源定位（DOA）、与 GAT 模块/上位机通信、RGB 指示。  
**ESP32 本地英文 MultiNet 唤醒已恢复**（AFE 无 AEC/WakeNet）；命中拉 GPIO47。GAT 仍负责 ASR 文本/TTS。

## 文件一览

| 文件 | 职责 |
|------|------|
| `DFR1266_VOREO.c` | 入口 `app_main`，编排启动顺序 |
| `UserConfig.h` | 板级宏：Codec、DOA 开关、GPIO、UART/I2C |
| `UserCode.h` / `UserCode.c` | 麦克风业务、DOA |
| `AudioInitialization.h` / `.c` | I2C + I2S + Codec（ES7210/ES7243E）驱动封装 |
| `PeripheralCode.h` / `.c` | RGB、GAT UART、用户 UART/I2C 从机、协议解析与数据链表 |
| `CMakeLists.txt` | 注册上述源文件 |
| `idf_component.yml` | 组件依赖（codec / esp-dsp / led_strip） |

---

## 启动流程（当前默认）

```text
app_main()
  ├─ initMIC()              // 打开 ES7210 四麦采集
  ├─ peripheralInit()       // RGB / GAT UART / MODE 选 UART 或 I2C / userTask
  ├─ // setRGBColor(RGB_BLACK)
  ├─ // autodetectSlotMapping()  // 调试用，默认关
  └─ initAudioDoa()         // LOCAL_DOA_ENABLE=1 时：doa_feed + audio_doa_task
```

Flash：**16MB**，含 `model` 分区（英文 MultiNet；见 `partitions.csv` / `README_MERGE_BIN.md`）。

---

## 模块说明

### 1. 入口 — `DFR1266_VOREO.c`

- 唯一公开入口：`app_main()`
- 由 `LOCAL_DOA_ENABLE` 决定是否启动 DOA

### 2. 板级配置 — `UserConfig.h`

| 配置项 | 含义 |
|--------|------|
| `ES7210` / `ES7243E` | 二选一 Codec；当前用 ES7210（4 路 TDM） |
| `LOCAL_DOA_ENABLE` | 1=开启声源定位，0=关闭 |
| `RGB_GPIO`、`MODE_GPIO` | 灯带、通信模式选择 |
| `GAT_*` | GAT 模组相关电源/唤醒/复位脚 |
| `USERUART_*` / `GTA_UART_*` | 用户侧与 GAT 侧串口引脚、波特率 |
| `USERI2C_ADDRESS` | I2C 从机地址 `0x1F` |

`MODE_GPIO` 电平决定用户侧走 **UART** 还是 **I2C 从机**（同一组 TX/SDA、RX/SCL 脚复用）。

### 3. 音频采集 — `AudioInitialization.*`

- `initAudio()`：创建 I2C 主控总线 + I2S（ES7210 为 TDM 四槽）+ `esp_codec_dev` 录音设备
- ES7210：仅 RX，无 TX
- 输出：`esp_codec_dev_handle_t`，供 `initMIC` / `doaFeedTask` 读 PCM

### 4. 麦克风与 DOA — `UserCode.*`

#### 4.1 麦克风初始化 `initMIC()`

- 调用 `initAudio()`，配置采样率/通道增益，打开 Codec

#### 4.2 DOA `initAudioDoa()` / `doaFeedTask` / `audioDoaTask`

- 创建 `audio_queue`，启动采麦任务与测向任务  
- 算法：时域 GCC / TDOA、Hann 窗等（见 `DOA_4MIC_RECTANGULAR.md`）  
- 参数：`SAMPLE_RATE=16k`，`FRAME_SIZE=1024`，`CHANNELS=4`，麦阵边长约 50mm  

#### 4.3 其它

- `autodetectSlotMapping()`：敲击各麦，估计 TDM 槽位与物理麦映射（调试）

### 5. 外设与协议 — `PeripheralCode.*`

#### 5.1 `peripheralInit()`

| 步骤 | 内容 |
|------|------|
| RGB | `led_strip` 初始化 |
| GAT 链表 | GAT ASR 文本等缓存 |
| GAT UART | UART1，115200，`gtaUartTask` 收 GAT 协议 |
| `userTask` | 周期性业务（如发角度/处理链表） |
| GPIO | MODE、GAT 电源/复位/唤醒等 |
| 用户通道 | MODE=I2C → I2C 从机；否则用户 UART |

#### 5.2 GAT 侧协议（UART1）

| 命令 | 值 | 含义 |
|------|-----|------|
| `GAT_CMD_WAKEUP` | `0xA0` | 唤醒 |
| `GAT_CMD_DETECTED` | `0xA1` | 检测 |
| `GAT_CMD_ASR_DATA` | `0xA2` | ASR 文本数据（模组侧） |
| `GAT_CMD_OPEN` | `0xA3` | 开机 |
| `GAT_CMD_READ_ERROR` | `0xAA` | 读错误 |

帧格式含头 `0xFD 0x00`、命令、长度、数据、结束 `0xFF`；大包用链表节点拼包。

#### 5.3 用户侧协议（UART 或 I2C）

| 命令 | 值 | 含义 |
|------|-----|------|
| `USER_UART_CMD_BEGIN` | `0x00` | 握手 |
| `USER_UART_CMD_QUERY_TEXT` | `0x01` | 查询是否有文本 |
| `USER_UART_CMD_GET_TEXT` | `0x02` | 取 GAT ASR 文本 |
| `USER_UART_CMD_SEND_TEXT` | `0x03` | 发文本（转 TTS） |
| `USER_UART_CMD_ANGLE` | `0x04` | 角度 |
| `USER_UART_CMD_DISTANCE` | `0x05` | 距离 |
| `USER_UART_CMD_SET_ASR` | `0x06` | 设置英文唤醒词（MultiNet grapheme） |
| `USER_UART_CMD_TTS_SPEED` | `0x07` | TTS 语速（1 字节，`0x00`~`0x0F`） |
| `USER_UART_CMD_GET_ASR_STATE` | `0x08` | ASR 状态 |
| `USER_UART_CMD_TTS_VOLUME` | `0x09` | TTS 音量（1 字节，`0x00`~`0x0F`） |

用户帧头 `0x55`，发送侧带 CRC16。应用帧 UART / I2C 共用。

**I2C 寄存器邮箱**（地址 `0x1F`）：

| 寄存器 | 值 | 方向 | 说明 |
|--------|-----|------|------|
| `STATUS` | `0x00` | R | Write 选中后 Read 3：`[status][len_h][len_l]` |
| `DATA` | `0x03` | R | Write 选中后流式 Read 应答帧 |
| `CTRL` | `0x04` | W | 写 `0x2F`=ABORT |
| `FIFO` | `0x10` | W | 写 `[0x10][应用帧…]`，可分包 |

STATUS：`0x00` IDLE，`0x01` BUSY，`0x02` READY，`0x03` ERROR。应答先写入从机静态缓冲再置 READY。

载荷：TTS≤128B；ASR≤4096B（`FD 00 A2+len+UTF-8+FF`，新帧覆盖）。

#### 5.4 RGB

- `setRGBColor(rgb_t)`：状态指示

---

## 任务一览

| 任务名 | 创建位置 | 作用 |
|--------|--------|------|
| `doa_feed` | `initAudioDoa` | 读 Codec → DOA 队列 |
| `audio_doa_task` | `initAudioDoa` | DOA 处理 |
| `gtaUartTask` | `peripheralInit` | 收 GAT 串口 |
| `userTask` | `peripheralInit` | 用户侧周期逻辑 |
| `userUartTask` | UART 模式 | 收用户串口并解析 |
| `i2cSlaveTask` | I2C 模式 | I2C 收发事件 |
| `i2cDataProcessTask` | I2C 模式 | 处理 I2C 收到的用户命令 |

---

## 模块连接关系

整体：**ESP32-S3 主控 + 麦阵 Codec / GAT / 用户口**。采麦与 DOA 在 `UserCode`；GAT 与用户侧经 `gatDataList` 交接模组 ASR 文本。

### 硬件连接（板级）

```text
                    ┌──────────────────────────────────────┐
                    │              ESP32-S3                │
  4×Mic ──I2S TDM──►│ GPIO3/4/5/6  + I2C(10/11)  ES7210   │
  RGB LED ──RMT────►│ GPIO9                                │
  MODE 键 ─────────►│ GPIO21  （用户口 UART / I2C）         │
  GAT 模组 ◄─UART1─►│ TX38 / RX39 @115200                  │
  上位机/用户 ◄────►│ GPIO14/13  UART2 或 I2C@0x1F         │
                    └──────────────────────────────────────┘
```

### 软件调用关系

```text
DFR1266_VOREO.c (app_main)
        ├──► UserCode.initMIC() → AudioInitialization.initAudio()
        ├──► PeripheralCode.peripheralInit()
        └──► UserCode.initAudioDoa()   // LOCAL_DOA_ENABLE
                  ├── doaFeedTask  ──读 Codec──► audio_queue
                  └── audioDoaTask ──► angle
```

### 运行时数据流

```text
Mic → ES7210 → doaFeedTask → audio_queue → audioDoaTask → angle
                                                    │
GAT UART（唤醒/ASR 文本）──► gatDataList ◄── 用户 UART/I2C
```

要点：

1. **采麦 → DOA**：只在 `UserCode` 内闭环，不依赖 AFE。  
2. **GAT → 链表**：模组 ASR 文本（`0xA2`）进入 `gatDataList`。  
3. **用户 ↔ 链表**：`GET_TEXT` 取 GAT 文本；`SEND_TEXT` 经 `userTask` 发给模组 TTS。  

---

## 启用可选功能

| 功能 | 改法 |
|------|------|
| DOA | `UserConfig.h` 中 `LOCAL_DOA_ENABLE`（1/0） |
| 麦槽自检 | `app_main` 取消注释 `autodetectSlotMapping()` |
| 换 Codec | `UserConfig.h` 改 `ES7210` / `ES7243E` |
| 用户改走 I2C | 硬件拉好 `MODE_GPIO` 对应电平 |

更细的 DOA 几何与算法说明见仓库根目录 `DOA_4MIC_RECTANGULAR.md`；固件合并烧录见 `README_MERGE_BIN.md`。
