# ECG 数据流控制与自适应输出模块文档 (`ecg_stream.c`)

本文档根据 [ecg_stream.c](file:///D:/Software_dev/STM32H750/LH001-91FIFO/Core/Src/ecg_stream.c) 转换生成，系统记录了双芯片（Master / Slave）心电采集系统中的数据重组、单 Slot 粒度环形缓冲、1ms 匀速调度输出、CRC16 校验以及自适应定时器水位动态调节控制逻辑。

---

## 目录
- [一、 模块架构与设计思想](#一-模块架构与设计思想)
- [二、 协议帧结构与线序规范](#二-协议帧结构与线序规范)
- [三、 核心机制与算法解析](#三-核心机制与算法解析)
  - [3.1 单 Slot 粒度环形缓冲设计](#31-单-slot-粒度环形缓冲设计)
  - [3.2 生产者与消费者模型](#32-生产者与消费者模型)
  - [3.3 自适应水位调节算法 (P 调节)](#33-自适应水位调节算法-p-调节)
  - [3.4 CRC16 校验算法](#34-crc16-校验算法)
- [四、 API 接口详解](#四-api-接口详解)
- [五、 完整源代码 (`ecg_stream.c`)](#五-完整源代码-ecg_streamc)

---

## 一、 模块架构与设计思想

`ecg_stream.c` 是心电数据流式传输的核心调度层，连接底层的双芯片 SPI FIFO 读取模块与上层的 UART DMA 发送链路：

```
+--------------------------------------------------------------------+
|  生产者 (EXTI 中断 / 主循环)：批次读取 LH001-91 主从芯片 FIFO (16 点)   |
+--------------------------------------------------------------------+
                                  |
                                  | ecg_stream_publish_slots()
                                  v
+--------------------------------------------------------------------+
|      Slot 环形缓冲区 (容量 64 Slots, 每个 Slot = Master 6B + Slave 6B)  |
|      目标水位: 24 Slots, 平滑低通滤波 avg_watermark                  |
+--------------------------------------------------------------------+
                                  |
                                  | ecg_stream_pump_slot() (1ms 定时器中断触发)
                                  v
+--------------------------------------------------------------------+
|      组装 22 字节单 Slot 帧 (对齐 32B Cache Line) + CRC16 校验        |
|      启动 UART DMA 异步发送                                          |
|      自适应微调 1ms 定时器 ARR 重装载值 (994us ~ 1004us)             |
+--------------------------------------------------------------------+
                                  |
                                  | UART DMA 发送完成中断
                                  v
+--------------------------------------------------------------------+
|      ecg_stream_notify_tx_done() 释放 tx_busy 标志                  |
+--------------------------------------------------------------------+
```

### 主要特性
1. **单 Slot 粒度解耦**：突发批次入队（如 16 点/批次），单 Slot 匀速出队（1ms/点），彻底平滑突发读取抖动。
2. **Cache Line 安全**：发送缓冲区 `tx_frame` 按 32 字节对齐，完全适配 STM32H7 的 D-Cache 与 AXI DMA。
3. **闭环自适应时钟校正**：通过监测环形缓冲区平均水位偏差，动态微调消费定时器 ARR 重装载值（±0.5% 范围内），抵消外部晶振频偏，防止缓冲区长期下溢或上溢。
4. **轻量原子保护**：发送权获取采用 `PRIMASK` 关中断原子保护，防止多任务/中断上下文并发竞争。

---

## 二、 协议帧结构与线序规范

单 Slot 帧固定长度为 **22 字节**，格式定义如下：

```
+----------+----------+----------+----------+----------+----------+----------+----------+
|  Byte 0  |  Byte 1  |  Byte 2  |  Byte 3  |  Byte 4  |  Byte 5  |  Byte 6  |  Byte 7  |
+----------+----------+----------+----------+----------+----------+----------+----------+
|   SOF0   |   SOF1   | VERSION  |SLOT_COUNT|           SEQ (32-bit LE)                 |
|  (0xA5)  |  (0x5A)  |  (0x01)  |  (0x01)  |      帧序号 (小端字节序，自增防丢包)         |
+----------+----------+----------+----------+----------+----------+----------+----------+
|  Byte 8 .. Byte 13 (6 Bytes)              |  Byte 14 .. Byte 19 (6 Bytes)            |
+-------------------------------------------+------------------------------------------+
|         Master 主机采样点 (6 字节)          |         Slave 从机采样点 (6 字节)          |
+-------------------------------------------+------------------------------------------+
|  Byte 20 (LSB)      |  Byte 21 (MSB)      |
+---------------------+---------------------+
|        CRC16 校验码 (小端字节序)            |
+-------------------------------------------+
```

- **CRC 校验范围**：从 `Byte 2 (VERSION)` 开始，覆盖到 `Byte 19 (PAYLOAD 末尾)`，共 18 字节。

---

## 三、 核心机制与算法解析

### 3.1 单 Slot 粒度环形缓冲设计

- **容量**：`ECG_SLOT_RING_CAPACITY = 64`（必须为 2 的整数次幂）。
- **快速索引取模**：采用位与运算替代求余取模：
  $$\text{index} = (\text{index} + 1) \ \& \ (\text{CAPACITY} - 1)$$
- **水位计算**：
  $$\text{Watermark} = (\text{write\_idx} + \text{CAPACITY} - \text{read\_idx}) \ \& \ (\text{CAPACITY} - 1)$$

### 3.2 生产者与消费者模型

- **生产者 (`ecg_stream_publish_slots`)**：
  - 接收 Master 和 Slave 的突发数据块（各 6 字节/点），按时间戳对应合并为 12 字节 Slot 写入环形缓冲区；
  - 缓冲区满时记录 `stats.overflow_count` 丢包计数；
  - 采用一阶低通滤波平滑当前水位：
    $$\text{avg\_watermark} = \text{avg\_watermark} \times 0.85 + \text{cur\_watermark} \times 0.15$$

- **消费者 (`ecg_stream_pump_slot`)**：
  - 在 1ms 定时器中断中被调用；
  - 检查缓冲区是否非空且 `tx_busy == 0`（通过 `__disable_irq()` 关中断原子抢占发送权）；
  - 取出 1 个 Slot，组装 22 字节帧，计算 CRC16；
  - 调用 `UART_Send_DMA_Safe` 启动 DMA 发送；
  - 执行自适应水位 ARR 动态调节。

### 3.3 自适应水位调节算法 (P 调节)

系统设计目标水位为 `ECG_TARGET_WATERMARK = 24` 个 Slots：

$$e(t) = \text{avg\_watermark} - 24.0$$
$$\text{target\_arr} = 999 - \lfloor e(t) \times 0.5 \rfloor$$

- **调节限幅**：严格限制在 `[994, 1004]` 范围之内（基准 1000us，±5us，即 ±0.5% 调频范围）；
- **动态更新**：若计算结果发生变化，则通过 `__HAL_TIM_SET_AUTORELOAD(st->htim_1ms, st->current_arr)` 实时重载定时器周期。

### 3.4 CRC16 校验算法

采用标准 CRC-16/CCITT 算法模型：
- **多项式**：`0x1021` ($x^{16} + x^{12} + x^5 + 1$)
- **初始值**：`0xFFFF`
- **处理方式**：MSB First 移位迭代。

---

## 四、 API 接口详解

### `ecg_stream_init`
- **原型**：`ecg_stream_status_t ecg_stream_init(ecg_stream_t *st, UART_HandleTypeDef *uart, TIM_HandleTypeDef *htim_1ms);`
- **说明**：初始化流管理结构体，清零统计变量，绑定 UART 句柄与 1ms 调度定时器，初始化目标水位与 ARR 基准值（999）。

### `ecg_stream_get_watermark`
- **原型**：`uint16_t ecg_stream_get_watermark(const ecg_stream_t *st);`
- **说明**：获取当前环形缓冲区内积压的 Slot 数量（有效数据深度）。

### `ecg_stream_publish_slots`
- **原型**：
  ```c
  ecg_stream_status_t ecg_stream_publish_slots(ecg_stream_t *st,
                                               const uint8_t *master_block,
                                               const uint8_t *slave_block,
                                               uint8_t slot_count);
  ```
- **说明**：生产者批量入队接口。将主片与从片采集到的数据点交叉配对组装并写入环形缓冲区，同时更新平滑滤波水位。

### `ecg_stream_pump_slot`
- **原型**：`ecg_stream_status_t ecg_stream_pump_slot(ecg_stream_t *st);`
- **说明**：消费者定时触发接口。原子检查发送状态并弹出 1 个 Slot，完成 CRC16 计算与 DMA 异步发送，并根据水位差动态微调 ARR。

### `ecg_stream_notify_tx_done` / `ecg_stream_notify_tx_error`
- **原型**：
  ```c
  ecg_stream_status_t ecg_stream_notify_tx_done(ecg_stream_t *st);
  ecg_stream_status_t ecg_stream_notify_tx_error(ecg_stream_t *st);
  ```
- **说明**：DMA 发送完成/错误回调通知接口。释放 `tx_busy` 发送标志并累加对应的统计计数。

---

## 五、 完整源代码 (`ecg_stream.c`)

以下为 [ecg_stream.c](file:///D:/Software_dev/STM32H750/LH001-91FIFO/Core/Src/ecg_stream.c) 的完整源代码：

```c
/* ecg_stream.c: 单 Slot 粒度环形缓冲、1ms 单 Slot 帧结构与定时器水位自适应调节 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "ecg_stream.h"
#include "usart.h"

enum {
    ECG_CRC16_INIT      = 0xFFFF,
    ECG_CRC16_POLY      = 0x1021,
    ECG_CRC16_MSB_MASK  = 0x8000,
    ECG_BITS_PER_BYTE   = 8,
    ECG_BYTE_MASK       = 0xFF
};

/* 编译期静态断言 */
#define ECG_STATIC_ASSERT(cond, name) \
    typedef char ecg_static_assert_##name[(cond) ? 1 : -1]

ECG_STATIC_ASSERT(offsetof(ecg_stream_t, tx_frame) == 0, tx_frame_first);
ECG_STATIC_ASSERT(sizeof(((ecg_stream_t *)0)->tx_frame) >= ECG_SINGLE_SLOT_FRAME_BYTES, tx_frame_capacity);
ECG_STATIC_ASSERT((ECG_SLOT_RING_CAPACITY & (ECG_SLOT_RING_CAPACITY - 1)) == 0, capacity_power_of_two);

static uint16_t ecg_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = ECG_CRC16_INIT;
    for (uint16_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)buf[i] << ECG_BITS_PER_BYTE));
        for (uint8_t bit = 0; bit < ECG_BITS_PER_BYTE; bit++) {
            uint16_t shifted = (uint16_t)(crc << 1);
            crc = ((crc & ECG_CRC16_MSB_MASK) != 0U)
                  ? (uint16_t)(shifted ^ ECG_CRC16_POLY)
                  : shifted;
        }
    }
    return crc;
}

ecg_stream_status_t ecg_stream_init(ecg_stream_t *st, UART_HandleTypeDef *uart, TIM_HandleTypeDef *htim_1ms)
{
    if (st == NULL || uart == NULL)
        return ECG_STREAM_ERR_ARG;

    memset(st, 0, sizeof(*st));
    st->uart = uart;
    st->htim_1ms = htim_1ms;
    st->avg_watermark = (float)ECG_TARGET_WATERMARK;
    st->current_arr = 999; // 初始 1000us 基准

    return ECG_STREAM_OK;
}

uint16_t ecg_stream_get_watermark(const ecg_stream_t *st)
{
    if (st == NULL)
        return 0;
    return (uint16_t)((st->write_idx + ECG_SLOT_RING_CAPACITY - st->read_idx) & (ECG_SLOT_RING_CAPACITY - 1));
}

ecg_stream_status_t ecg_stream_publish_slots(ecg_stream_t *st,
                                             const uint8_t *master_block,
                                             const uint8_t *slave_block,
                                             uint8_t slot_count)
{
    if (st == NULL || master_block == NULL || slave_block == NULL || slot_count == 0)
        return ECG_STREAM_ERR_ARG;

    for (uint8_t i = 0; i < slot_count; i++) {
        uint16_t next_w = (uint16_t)((st->write_idx + 1U) & (ECG_SLOT_RING_CAPACITY - 1U));
        if (next_w == st->read_idx) {
            // 环形缓冲区满
            st->stats.overflow_count++;
            break;
        }

        uint8_t *slot_dst = st->slot_ring[st->write_idx];
        memcpy(&slot_dst[0], &master_block[i * ECG_SAMPLE_BYTES], ECG_SAMPLE_BYTES);
        memcpy(&slot_dst[ECG_SAMPLE_BYTES], &slave_block[i * ECG_SAMPLE_BYTES], ECG_SAMPLE_BYTES);

        st->write_idx = next_w;
        st->stats.slots_published++;
    }

    // 更新平滑水位 (滤波更新)
    uint16_t cur_wm = ecg_stream_get_watermark(st);
    st->avg_watermark = st->avg_watermark * 0.85f + (float)cur_wm * 0.15f;

    return ECG_STREAM_OK;
}

ecg_stream_status_t ecg_stream_pump_slot(ecg_stream_t *st)
{
    if (st == NULL)
        return ECG_STREAM_ERR_ARG;

    // 检查缓冲空
    if (st->write_idx == st->read_idx)
        return ECG_STREAM_IDLE;

    // 原子抢占发送权
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint8_t granted = 0;
    if (st->tx_busy == 0U) {
        st->tx_busy = 1;
        granted = 1;
    }

    __set_PRIMASK(primask);

    if (!granted)
        return ECG_STREAM_IDLE;

    // 弹出一个 Slot
    uint16_t cur_r = st->read_idx;
    uint8_t *slot_src = st->slot_ring[cur_r];
    st->read_idx = (uint16_t)((cur_r + 1U) & (ECG_SLOT_RING_CAPACITY - 1U));

    // 组装 22 字节单 Slot 帧
    uint8_t *frame = st->tx_frame;
    uint32_t seq = st->next_seq++;

    frame[ECG_OFF_SOF0]       = ECG_SOF0;
    frame[ECG_OFF_SOF1]       = ECG_SOF1;
    frame[ECG_OFF_VERSION]    = ECG_FRAME_VERSION;
    frame[ECG_OFF_SLOT_COUNT] = 1; // 单 Slot
    frame[ECG_OFF_SEQ + 0]    = (uint8_t)(seq & ECG_BYTE_MASK);
    frame[ECG_OFF_SEQ + 1]    = (uint8_t)((seq >> 8) & ECG_BYTE_MASK);
    frame[ECG_OFF_SEQ + 2]    = (uint8_t)((seq >> 16) & ECG_BYTE_MASK);
    frame[ECG_OFF_SEQ + 3]    = (uint8_t)((seq >> 24) & ECG_BYTE_MASK);

    memcpy(&frame[ECG_OFF_PAYLOAD], slot_src, ECG_SLOT_BYTES);

    // 计算从 VERSION 起至 Payload 末尾 (18 字节) 的 CRC16
    uint16_t crc_span = ECG_FRAME_HEADER_BYTES + ECG_SLOT_BYTES;
    uint16_t crc = ecg_crc16(&frame[ECG_OFF_VERSION], (uint16_t)(crc_span - ECG_OFF_VERSION));

    frame[crc_span]      = (uint8_t)(crc & ECG_BYTE_MASK);
    frame[crc_span + 1U] = (uint8_t)((crc >> 8) & ECG_BYTE_MASK);

    // 启动 UART DMA 发送 (发送 22 字节)
    if (UART_Send_DMA_Safe(st->uart, frame, ECG_SINGLE_SLOT_FRAME_BYTES) != HAL_OK) {
        st->stats.tx_error_count++;
        st->tx_busy = 0;
        return ECG_STREAM_ERR_UART;
    }

    // -------------------------------------------------------------
    // 自适应水位调节逻辑 (微调 1ms 定时器重装载值 ARR)
    // -------------------------------------------------------------
    float err = st->avg_watermark - (float)ECG_TARGET_WATERMARK;
    // P 调节：水位偏高则减小 ARR（如 995us 加速消费），水位偏低则增大 ARR（如 1003us 减缓消费）
    int32_t target_arr = 999 - (int32_t)(err * 0.5f);

    // 限幅控制在 994 ~ 1004 之间 (±0.5% 微调范围)
    if (target_arr > 1004) target_arr = 1004;
    if (target_arr < 994)  target_arr = 994;

    if ((uint16_t)target_arr != st->current_arr && st->htim_1ms != NULL) {
        st->current_arr = (uint16_t)target_arr;
        __HAL_TIM_SET_AUTORELOAD(st->htim_1ms, st->current_arr);
    }

    return ECG_STREAM_OK;
}

ecg_stream_status_t ecg_stream_notify_tx_done(ecg_stream_t *st)
{
    if (st == NULL)
        return ECG_STREAM_ERR_ARG;

    if (st->tx_busy == 1U) {
        st->stats.slots_sent++;
        st->tx_busy = 0;
    }
    return ECG_STREAM_OK;
}

ecg_stream_status_t ecg_stream_notify_tx_error(ecg_stream_t *st)
{
    if (st == NULL)
        return ECG_STREAM_ERR_ARG;

    if (st->tx_busy == 1U) {
        st->stats.tx_error_count++;
        st->tx_busy = 0;
    }
    return ECG_STREAM_OK;
}
```
