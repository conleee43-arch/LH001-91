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
