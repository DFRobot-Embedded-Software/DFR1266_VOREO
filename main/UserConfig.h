#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#define ES7210
//#define ES7243E

#define LOCAL_DOA_ENABLE                1 // 1=开启声源定位(DOA)，0=关闭


#define RGB_GPIO                        9 //RGB指示灯引脚
#define RGB_CIRCLE_GPIO                 48 //RGB圆环指示灯引脚

#define MODE_GPIO                       21 //通信模式选择按键引脚

#define GAT_WAKEUP_ASR_GPIO             47 //语音识别功能唤醒同WAKEUP按键

#define GAT_VDD_EN_GPIO                 7 //GAT电源使能引脚，默认输出高
#define GAT_RESET_GPIO                  45 //GAT复位引脚，输入不上拉
#define GAT_SPI0_DO_GPIO                12 //未使用
#define GAT_WAKEUP_GPIO                 8 //低功耗唤醒引脚，默认输出高

#define USERUART_PORT                   UART_NUM_2 //gravity接口
#define USERUART_TX_SDA_GPIO            14
#define USERUART_RX_SCL_GPIO            13

#define GTA_UART_PORT                   UART_NUM_1 //GTA接口
#define GTA_UART_TX_GPIO                38
#define GTA_UART_RX_GPIO                39

#define SNR9912_UART_PORT               UART_NUM_0 //SNR9912VR UART串口
#define SNR9912_UART_TX_GPIO            43
#define SNR9912_UART_RX_GPIO            44
#define SNR9912_UART_BAUDRATE           9600

#define UART_BAUDRATE                   9600
#define GTA_UART_BAUDRATE               115200
#define UART_BUF_SIZE                   1024

#define USERI2C_ADDRESS                 0x1f
#define I2C_SLAVE_NUM                   I2C_NUM_1



#endif
