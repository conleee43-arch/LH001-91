/**
  ******************************************************************************
  * @file           : ecg_stream.h
  * @brief          : 主从采样点时域交叉重组、成帧与 UART 流式输出接口
  ******************************************************************************
  */

#ifndef __ECG_STREAM_H
#define __ECG_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>

#include "main.h"

/*******************************************************************************
 * 线序常量 (Wire Format Constants)
 *
 * 帧结构 (小端, 长度随 slot_count 变化):
 *   [0]      SOF0                = 0xA5
 *   [1]      SOF1               = 0x5A
 *   [2]      VERSION             = 0x01
 *   [3]      SLOT_COUNT          = 本帧时间槽数, 上限 ECG_FRAME_MAX_SLOTS
 *   [4..7]   SEQ (uint32 LE)     = 帧序号, 上位机据此发现丢帧
 *   [8..]    PAYLOAD             = slot_count 个时间槽, 每槽 12 字节:
 *                                  主机 6 字节, 紧随其后从机 6 字节
 *   [尾部 2] CRC16 (LE)          = 覆盖 [2] 起至 PAYLOAD 末尾
 *
 * 交叉重组语义: 第 i 个时间槽内的主从两个采样点采自同一个 1ms 时刻。
 ******************************************************************************/
enum {
    ECG_SAMPLE_BYTES        = 6,
    ECG_CHIP_COUNT          = 2,
    ECG_SLOT_BYTES          = ECG_SAMPLE_BYTES * ECG_CHIP_COUNT, // 12 Bytes
    ECG_FRAME_HEADER_BYTES  = 8,
    ECG_FRAME_CRC_BYTES     = 2,
    ECG_SINGLE_SLOT_FRAME_BYTES = ECG_FRAME_HEADER_BYTES + ECG_SLOT_BYTES + ECG_FRAME_CRC_BYTES, // 22 Bytes
    ECG_CACHE_LINE_BYTES    = 32,
    ECG_SLOT_RING_CAPACITY  = 64, // Must be power of 2
    ECG_TARGET_WATERMARK    = 24  // Target watermark level in Slots
};

enum {
    ECG_SOF0            = 0xA5,
    ECG_SOF1            = 0x5A,
    ECG_FRAME_VERSION   = 0x01
};

/* 帧内字段偏移 */
enum {
    ECG_OFF_SOF0        = 0,
    ECG_OFF_SOF1        = 1,
    ECG_OFF_VERSION     = 2,
    ECG_OFF_SLOT_COUNT  = 3,
    ECG_OFF_SEQ         = 4,
    ECG_OFF_PAYLOAD     = ECG_FRAME_HEADER_BYTES
};

/*******************************************************************************
 * 类型定义
 ******************************************************************************/
typedef enum {
    ECG_STREAM_OK        = 0,
    ECG_STREAM_IDLE      = 1,   /* 无事可做: 发送在途或环形缓冲为空 */
    ECG_STREAM_ERR_ARG   = -1,
    ECG_STREAM_ERR_FULL  = -2,
    ECG_STREAM_ERR_UART  = -3
} ecg_stream_status_t;

typedef struct {
    uint32_t slots_published;   /* 成功入环的 Slot 数 */
    uint32_t slots_sent;        /* DMA 发送完成的 Slot 数 */
    uint32_t overflow_count;    /* 环形缓冲满而丢弃的 Slot 数 */
    uint32_t tx_error_count;    /* UART 发送失败次数, 该帧会被重发 */
} ecg_stream_stats_t;

/*
 * 单 Slot 粒度环形缓冲与 1ms 匀速自适应输出控制结构
 */
typedef struct {
    /* 单帧 DMA 发送缓冲区（按 32 字节对齐，符合 Cache Line 要求） */
    uint8_t tx_frame[ECG_CACHE_LINE_BYTES] __attribute__((aligned(ECG_CACHE_LINE_BYTES)));

    /* Slot 环形缓冲区 (64 * 12 字节) */
    uint8_t slot_ring[ECG_SLOT_RING_CAPACITY][ECG_SLOT_BYTES];

    UART_HandleTypeDef *uart;   /* 借用 UART 句柄 */
    TIM_HandleTypeDef  *htim_1ms;/* 1ms 匀速消费定时器 */
    
    uint32_t            next_seq;
    volatile uint16_t   write_idx;
    volatile uint16_t   read_idx;
    volatile uint8_t    tx_busy;
    
    /* 水位线平滑与自适应调节 */
    volatile float      avg_watermark;
    volatile uint16_t   current_arr;
    
    ecg_stream_stats_t  stats;
} ecg_stream_t;

/*******************************************************************************
 * API
 ******************************************************************************/

/* 初始化流实例，绑定 UART 与 1ms 定时器 */
ecg_stream_status_t ecg_stream_init(ecg_stream_t *st, UART_HandleTypeDef *uart, TIM_HandleTypeDef *htim_1ms);

/* 生产端：将主从芯片突发读取到的 slot_count 个时间槽批量推入 Slot 环形缓冲 */
ecg_stream_status_t ecg_stream_publish_slots(ecg_stream_t *st,
                                             const uint8_t *master_block,
                                             const uint8_t *slave_block,
                                             uint8_t slot_count);

/* 消费端：1ms 定时器中断触发，弹出一个 Slot 并通过 UART DMA 发送，同时微调 TIM 重装载值 */
ecg_stream_status_t ecg_stream_pump_slot(ecg_stream_t *st);

/* 通告一帧发送完成：释放发送权 */
ecg_stream_status_t ecg_stream_notify_tx_done(ecg_stream_t *st);

/* 通告发送失败：释放发送权 */
ecg_stream_status_t ecg_stream_notify_tx_error(ecg_stream_t *st);

/* 获取当前缓冲区暂存的 Slot 数量 */
uint16_t ecg_stream_get_watermark(const ecg_stream_t *st);

#ifdef __cplusplus
}
#endif

#endif /* __ECG_STREAM_H */
