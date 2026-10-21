#ifndef PERIPHERAL_CODE_H
#define PERIPHERAL_CODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rgb.h"
#include "UserConfig.h"
#include "UserCode.h"



#define UART_MODE                       0
#define I2C_MODE                        1

#define RGB_BLUE                        rgb_from_code(0x00ff)
#define RGB_GREEN                       rgb_from_code(0xff00)
#define RGB_BLACK                       rgb_from_code(0x00)
#define RGB_YELLOW                      rgb_from_code(0xffff00)
#define RGB_CYAN                        rgb_from_code(0x00ffff)
#define RGB_RED                         rgb_from_code(0xff0000)



#define UART_READ_BUF_SIZE              1024
#define UART_DATA_BITS                  UART_DATA_8_BITS
#define UART_STOP_BITS                  UART_STOP_BITS_1
#define UART_PARITY                     UART_PARITY_DISABLE
#define UART_FLOW_CTRL                  UART_HW_FLOWCTRL_DISABLE
#define UART_SOURCE_CLK                 UART_SCLK_DEFAULT



#define GAT_UART_CMD_HEAD0               (0)
#define GAT_UART_CMD_HEAD1               (1)
#define UART_CMD_CMD                     (2)
#define UART_CMD_LEN_H                   (3)
#define UART_CMD_LEN_L                   (4)
#define GAT_UART_CMD_DATA                (5)
#define GAT_UART_CMD_END                  (6)
#define USER_UART_CMD_DATA                (7)

/** 模组→主机：帧头 FD + 00 + 命令 + FF */
#define GAT_CMD_HEAD0                    (0xFD)  //头0（模组→主机）
#define GAT_CMD_HEAD1                    (0x00)  //头1
#define GAT_CMD_END                      (0xFF) //结束符
#define GAT_CMD_WAKEUP                   (0xA0) //唤醒通知
#define GAT_CMD_DETECTED                 (0xA1) //识别完成
#define GAT_CMD_ASR_DATA                 (0xA2) //ASR文本
#define GAT_CMD_OPEN                     (0xA3) //GTA初始化完成后应答
#define GAT_CMD_ASR_TIMEOUT              (0xA4) //ASR超时
#define GAT_CMD_SPEED_ACK                (0xA5) //语速设置应答
#define GAT_CMD_VOLUME_ACK               (0xA6) //音量设置应答
#define GAT_CMD_READ_ERROR               (0xAA) //读取错误

/** 主机→模组：帧头 DF + 命令 + 数据 + FF */
#define GAT_HOST_HEAD                    (0xDF)
#define GAT_HOST_CMD_TTS                 (0xA0) // TTS 播报内容
#define GAT_HOST_CMD_VOLUME              (0xA1) // 音量 0x00~0x0F
#define GAT_HOST_CMD_SPEED               (0xA2) // 语速 0x00~0x0F


#define USER_UART_CMD_HEAD               (0x55)
#define USER_UART_CMD_END                (0xFF)
#define USER_UART_CMD_BEGIN              (0x00) //握手命令
#define USER_UART_CMD_QUERY_TEXT         (0x01) //查询文本命令
#define USER_UART_CMD_GET_TEXT           (0x02) //获取文本命令
#define USER_UART_CMD_SEND_TEXT          (0x03) //发送文本命令
#define USER_UART_CMD_ANGLE              (0x04) //角度命令
#define USER_UART_CMD_DISTANCE           (0x05) //距离命令
#define USER_UART_CMD_SET_ASR            (0x06) //设置ASR命令
#define USER_UART_CMD_TTS_SPEED          (0x07) //TTS速度命令 0x00~0x0F
#define USER_UART_CMD_GET_ASR_STATE      (0x08) //获取ASR状态命令
#define USER_UART_CMD_TTS_VOLUME         (0x09) //TTS音量命令 0x00~0x0F


/*
 * I2C 寄存器邮箱（Master Write: [reg][data…]；Write [reg] 后 Read）
 *  0x00 STATUS  R  读 1 或连读 3：[status][len_h][len_l]
 *  0x01 LEN_H   R
 *  0x02 LEN_L   R
 *  0x03 DATA    R  应答帧流式读（偏移由从机维护）
 *  0x04 CTRL    W  写 0x2F=ABORT
 *  0x10 FIFO    W  主机写入应用帧 0x55|LEN|CMD|DATA|CRC（可分包）
 */
#define VOREO_REG_STATUS                  (0x00)
#define VOREO_REG_LEN_H                   (0x01)
#define VOREO_REG_LEN_L                   (0x02)
#define VOREO_REG_DATA                    (0x03)
#define VOREO_REG_CTRL                    (0x04)
#define VOREO_REG_FIFO                    (0x10)
#define VOREO_REG_CTRL_ABORT              (0x2F)

/* Link STATUS；勿用 I2C_STATUS_*（与 ESP-IDF 冲突） */
#define VOREO_I2C_LINK_IDLE               (0x00)
#define VOREO_I2C_LINK_BUSY               (0x01)
#define VOREO_I2C_LINK_READY              (0x02)
#define VOREO_I2C_LINK_ERROR              (0x03)

#define VOREO_TTS_PAYLOAD_MAX             (128)   /* TTS 单包最大字节（UTF-8） */
#define VOREO_ASR_PAYLOAD_MAX             (4096)  /* ASR 单帧最大字节（UTF-8） */

#define I2C_APP_FRAME_MAX                 (VOREO_ASR_PAYLOAD_MAX + 6)
#define I2C_TX_CHUNK_MAX                  (128)  /* 邮箱 DATA 单次泵送上限 */
#define I2C_RX_EVENT_MAX                  (256)

#define I2C_SLAVE_SEND_STATIC_OK          (0x30)
#define I2C_SLAVE_SEND_STATIC_ERROR       (0x31)
#define I2C_SLAVE_SEND_STATIC_LEN         (0x32)
#define I2C_SLAVE_SEND_STATIC_DATA        (0x33)

// GAT UART数据缓冲区最大大小
#define GAT_DATA_BUFFER_MAX_SIZE         (4096)

/**
 * @brief GAT UART数据包结构
 */
typedef struct gat_data_node {
    uint8_t cmd;                    // 命令类型
    uint16_t data_len;              // 数据长度
    uint8_t *data;                  // 数据缓冲区
    uint16_t received_len;          // 当前已接收的数据长度
    struct gat_data_node *next;     // 指向下一个节点的指针
} gat_data_node_t;

/**
 * @brief GAT数据链表结构
 */
typedef struct {
    gat_data_node_t *head;          // 链表头节点
    gat_data_node_t *tail;           // 链表尾节点
    uint32_t count;                  // 链表中的节点数量
} gat_data_list_t;

/**
 * @brief 初始化GAT数据链表
 * @return 返回初始化后的链表指针，失败返回NULL
 */
gat_data_list_t* gatDataListInit(void);

/**
 * @brief 创建新的GAT数据节点
 * @param cmd 命令类型
 * @param data_len 期望的数据长度
 * @return 返回新节点的指针，失败返回NULL
 */
gat_data_node_t* gatDataNodeCreate(uint8_t cmd, uint16_t data_len);

/**
 * @brief 向链表添加节点
 * @param list 链表指针
 * @param node 要添加的节点指针
 * @return 成功返回0，失败返回-1
 */
int gatDataListAdd(gat_data_list_t *list, gat_data_node_t *node);

/**
 * @brief 向节点追加数据
 * @param node 节点指针
 * @param data 要追加的数据指针
 * @param len 要追加的数据长度
 * @return 返回实际追加的字节数，错误返回-1
 */
int gatDataNodeAppend(gat_data_node_t *node, const uint8_t *data, size_t len);

/**
 * @brief 检查节点是否完整（所有数据已接收）
 * @param node 节点指针
 * @return 完整返回true，否则返回false
 */
bool gatDataNodeIsComplete(gat_data_node_t *node);

/**
 * @brief 从链表中移除并释放节点
 * @param list 链表指针
 * @param node 要移除的节点指针
 * @return 成功返回0，失败返回-1
 */
int gatDataListRemove(gat_data_list_t *list, gat_data_node_t *node);

/**
 * @brief 移除并释放链表中所有指定 cmd 的节点（ASR 新帧从头存储）
 */
void gatDataListRemoveAllCmd(gat_data_list_t *list, uint8_t cmd);

/**
 * @brief 获取第一个未完成的节点（用于接收数据）
 * @param list 链表指针
 * @return 返回第一个未完成节点的指针，全部完成则返回NULL
 */
gat_data_node_t* gatDataListGetIncomplete(gat_data_list_t *list);

/**
 * @brief 获取第一个已完成的节点（用于处理数据）
 * @param list 链表指针
 * @return 返回第一个已完成节点的指针，无完成节点则返回NULL
 */
gat_data_node_t* gatDataListGetComplete(gat_data_list_t *list);

/**
 * @brief 释放数据节点
 * @param node 要释放的节点指针
 */
void gatDataNodeFree(gat_data_node_t *node);

/**
 * @brief 释放链表中的所有节点
 * @param list 链表指针
 */
void gatDataListFree(gat_data_list_t *list);

/**
 * @brief 获取链表中的节点数量
 * @param list 链表指针
 * @return 返回节点数量
 */
uint32_t gatDataListGetCount(gat_data_list_t *list);

//初始化外设
int peripheralInit(void);

//设置RGB灯颜色
int setRGBColor(rgb_t color);

/** GTA 是否已上报开机 (0xA3) */
bool gatIsOpen(void);

/** 向 GTA 发送 TTS 文本（帧：DF A0 + UTF-8 + FF） */
void sendTextToGAT(const char *text);

/** 向 GTA 设置音量（帧：DF A1 + vol + FF，vol=0x00~0x0F） */
void setGatVolume(uint8_t volume);

/** 向 GTA 设置语速（帧：DF A2 + speed + FF，speed=0x00~0x0F） */
void setGatSpeed(uint8_t speed);

#endif
