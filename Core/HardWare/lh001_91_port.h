#ifndef __LH001_91_PORT_H
#define __LH001_91_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"
#include "main.h"

/* 1. SPI 片选使能宏定义 */
#define SPI_CLR_CS1()             HAL_GPIO_WritePin(SPI_CS1_GPIO_Port, SPI_CS1_Pin, GPIO_PIN_RESET)
#define SPI_SET_CS1()             HAL_GPIO_WritePin(SPI_CS1_GPIO_Port, SPI_CS1_Pin, GPIO_PIN_SET)

#define SPI_CLR_CS2()             HAL_GPIO_WritePin(SPI_CS2_GPIO_Port, SPI_CS2_Pin, GPIO_PIN_RESET)
#define SPI_SET_CS2()             HAL_GPIO_WritePin(SPI_CS2_GPIO_Port, SPI_CS2_Pin, GPIO_PIN_SET)

// 兼容单芯片宏定义，默认选通主芯片 CS1
#define SPI_CLR_CS()              SPI_CLR_CS1()
#define SPI_SET_CS()              SPI_SET_CS1()

/* 2. START 引脚控制宏定义 */
#define LH001_91_START_SET()     HAL_GPIO_WritePin(Start_GPIO_Port, Start_Pin, GPIO_PIN_SET)
#define LH001_91_START_CLR()     HAL_GPIO_WritePin(Start_GPIO_Port, Start_Pin, GPIO_PIN_RESET)

/* 3. RESET 硬件复位引脚控制 */
#define LH001_91_PDEXTRST_SET()   HAL_GPIO_WritePin(ECG_Rset_GPIO_Port, ECG_Rset_Pin, GPIO_PIN_SET)
#define LH001_91_PDEXTRST_CLR()   HAL_GPIO_WritePin(ECG_Rset_GPIO_Port, ECG_Rset_Pin, GPIO_PIN_RESET)

/* 4. 软件片选控制使能与哑字节 */
#define SOFTWARE_SPI_CS           1
#define LH001_91_SPI_TX_DUMMY     0x00

/* 5. 10微秒硬件定时器延时函数 (使用 TIM17) */
extern TIM_HandleTypeDef htim17;

void LH001_91_DELAY_INIT(void);

static inline void LH001_91_DELAY_10US(void)
{
    /* 1. 计数器清零 */
    htim17.Instance->CNT = 0;
    
    /* 2. 直接操作 CR1 寄存器启动定时器 (替代 HAL 库抽象开销) */
    htim17.Instance->CR1 |= TIM_CR1_CEN;
    
    /* 3. 阻塞等待 10us，增加超时防御计数，防止定时器异常导致死锁 */
    uint32_t timeout = 100000;
    while ((htim17.Instance->CNT < 10) && (--timeout > 0))
    {
        // 阻塞等待
    }
    
    /* 4. 直接操作 CR1 寄存器停止定时器 */
    htim17.Instance->CR1 &= ~TIM_CR1_CEN;
}

/* 6. SPI 全双工字节收发 */
extern SPI_HandleTypeDef hspi1;

static inline uint8_t stm_lh_spi_txrx8bit(uint8_t tx_byte)
{
    uint8_t rx_byte = 0;
    // 调用 HAL 库同步全双工收发，超时 5ms 以防总线阻塞
    if (HAL_SPI_TransmitReceive(&hspi1, &tx_byte, &rx_byte, 1, 5) != HAL_OK)
    {
        return 0xFF; // 出错或超时返回 0xFF
    }
    return rx_byte;
}

// 映射到驱动上层的宏
#define LH001_91_SpiTxRxByte(tx)  stm_lh_spi_txrx8bit(tx)

#ifdef __cplusplus
}
#endif

#endif /* __LH001_91_PORT_H */
