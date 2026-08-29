/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lh001_91_port.h"
#include "LH001_91.h"
#include "ecg_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define LH001_91_FIFO_THRESHOLD    16U

/* DRDY 待处理突发的排队上限: 当前正在处理的一个 + 排队一个。
 * 超出即说明消费跟不上生产, 计入 fifo_reentry_drop_count。 */
#define DRDY_PENDING_MAX           2U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 帧环形缓冲按 Cache Line 对齐, 使每帧都是独立的可安全维护的 DMA 源。
 * 实例位于默认 RW 区 (D1 AXI SRAM, 0x24000000), DMA1 可经 AHB-AXI 桥访问。 */
ecg_stream_t ecg_stream __attribute__((aligned(ECG_CACHE_LINE_BYTES)));

/* 断言失败现场, 供调试器读取。volatile 防止被优化掉。 */
volatile const char *assert_fail_expr = NULL;
volatile const char *assert_fail_file = NULL;
volatile int assert_fail_line = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
static void LH001_91_App_Init(void);
void UART_SendString(const char* str);

/* 复位两片 FIFO, 使主从采样点重新对齐到同一批次边界。 */
static void fifo_recover(void);

/* 取走一个待处理的 DRDY 突发。有则返回 1 并递减计数, 无则返回 0。 */
static uint8_t drdy_take_pending(void);

/* 两片突发均读取成功且点数一致且非空时返回 1。纯函数。 */
static uint8_t fifo_burst_is_valid(const LH001_91_FIFO_BURST_t *master,
                                  const LH001_91_FIFO_BURST_t *slave);

/* 读取一次主从突发并交叉重组入流, 失败则复位两片 FIFO。 */
static void fifo_service_once(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
volatile uint8_t fifo_irq_pending = 0;
volatile uint32_t drdy1_irq_count = 0;
volatile uint32_t fifo_reentry_drop_count = 0;

LH001_91_FIFO_BURST_t master_burst;
LH001_91_FIFO_BURST_t slave_burst;

uint32_t fifo_ok_count = 0;
uint32_t fifo_error_count = 0;
uint32_t fifo_recovery_count = 0;

static void fifo_recover(void)
{
    fifo_recovery_count++;
    lh001_91_reset_fifo_cmd();
    lh001_91_reset_fifo_hw();
}

static uint8_t drdy_take_pending(void)
{
    /* fifo_irq_pending 由 EXTI 中断递增, 递减必须原子 */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint8_t taken = 0;
    if (fifo_irq_pending > 0U) {
        fifo_irq_pending--;
        taken = 1;
    }

    __set_PRIMASK(primask);
    return taken;
}

static uint8_t fifo_burst_is_valid(const LH001_91_FIFO_BURST_t *master,
                                  const LH001_91_FIFO_BURST_t *slave)
{
    if (master->status != LH001_91_FIFO_READ_OK)
        return 0;
    if (slave->status != LH001_91_FIFO_READ_OK)
        return 0;
    if (master->count != slave->count)
        return 0;
    if (master->count == 0U)
        return 0;
    return 1;
}

static void fifo_service_once(void)
{
    lh001_91_read_fifo_burst_dual_num(LH001_91_CHIP_MASTER, &master_burst,
                                      LH001_91_FIFO_THRESHOLD);
    lh001_91_read_fifo_burst_dual_num(LH001_91_CHIP_SLAVE, &slave_burst,
                                      LH001_91_FIFO_THRESHOLD);

    if (!fifo_burst_is_valid(&master_burst, &slave_burst)) {
        fifo_error_count++;
        fifo_recover();
        return;
    }

    fifo_ok_count++;
    (void)ecg_stream_publish_slots(&ecg_stream, master_burst.data_buf,
                                   slave_burst.data_buf, master_burst.count);
}

int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, 2);
    return ch;
}

/**
  * @brief  LH001-91 双芯片应用层初始化序列
  */
static void LH001_91_App_Init(void)
{
    uint8_t master_id = 0, slave_id = 0;
    uint8_t master_regs[18], slave_regs[18];

    LH001_91_DELAY_INIT();
    lh001_91_hw_reset();

    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_CHIPID, &master_id, 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_CHIPID, &slave_id, 1);

    // 1. 配置 CLKCON 寄存器 (主从机配置 CLKCON=0x07)
    uint8_t userkey_val = LH001_91_USERKEY;
    uint8_t clkcon_val = MSK_LH001_91_CLKCON_CLKEN | MSK_LH001_91_CLKCON_F2M | MSK_LH001_91_CLKCON_PU; // 0x07

    lh001_91_write_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_USERKEY, &userkey_val, 1);
    lh001_91_write_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_CLKCON, &clkcon_val, 1);

    lh001_91_write_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_USERKEY, &userkey_val, 1);
    lh001_91_write_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_CLKCON, &clkcon_val, 1);

    // 2. 初始化导联脱落检测 (LOCON2 = 0x18, LOCON3 = 0x00)
    LH001_91_LEADOFF_t leadoff_init;
    leadoff_init.threshold = LH001_91_LEADOFF_DELTA_4;
    leadoff_init.current = LH001_91_LEADOFF_CURRENT_SOURCE_50nA;
    leadoff_init.sink_cfg = LH001_91_LEADOFF_SINK_CLOSE;
    leadoff_init.source_cfg = LH001_91_LEADOFF_SOURCE_CLOSE;
    leadoff_init.rld_sink_en = 0;
    leadoff_init.ain0_detect_en = 0;
    leadoff_init.ain1_detect_en = 0;
    leadoff_init.rld_detect_en = 0;

    lh001_91_leadoff_init_dual(LH001_91_CHIP_MASTER, &leadoff_init);
    lh001_91_leadoff_init_dual(LH001_91_CHIP_SLAVE, &leadoff_init);

    // 3. 初始化主从机右腿驱动 RLDCON (主机=0x05, 从机=0x01)
    lh001_91_rld_init_dual(LH001_91_CHIP_MASTER, 1, ENUM_LH001_91_RLD_REF_AVDD_DIV2, ENUM_LH001_91_RLD_CHOP_FMOD_DIV32);
    lh001_91_rld_init_dual(LH001_91_CHIP_SLAVE, 0, ENUM_LH001_91_RLD_REF_AVDD_DIV2, ENUM_LH001_91_RLD_CHOP_FMOD_DIV32);

    // 4. 初始化 PGA (PGAGAIN=0x04, PGACTRL=0x08, PGACTRL1=0x06)
    LH001_91_PGA_INIT_t pga_init;
    pga_init.gain = ENUM_LH001_91_PGA_PGAGAIN_6;
    pga_init.pga_bypass = 0;
    pga_init.pga_chop = ENUM_LH001_91_PGA_CHOP_DIV16;
    pga_init.power_mode = ENUM_LH001_91_PGA_POWER_LP;
    pga_init.comm_sense_en = 1;

    lh001_91_pga_init_dual(LH001_91_CHIP_MASTER, &pga_init);
    lh001_91_pga_init_dual(LH001_91_CHIP_SLAVE, &pga_init);

    // 5. 初始化 ADC (CONFIG1=0x01, MODCLK=0x10, BUFCON:主机=0x35/从机=0x05, ADCCTRL=0x03)
    LH001_91_ADC_INIT_t adc_init;
    adc_init.Fmodclk = ENUM_LH001_91_ADC_FMODCLK_128K;
    adc_init.oversample_rate = ENUM_LH001_91_ADC_DR_DIV128;
    adc_init.conv_mode = ENUM_LH001_91_ADC_CONTINOUS;
    adc_init.ref_sel = ENUM_LH001_91_ADC_REF_2V5;

    lh001_91_adc_init_dual(LH001_91_CHIP_MASTER, &adc_init);
    lh001_91_adc_init_dual(LH001_91_CHIP_SLAVE, &adc_init);
    
    // 显式配置从机 BUFCON=0x05
    uint8_t bufcon_val = MSK_LH001_91_BUFCON_AVDD_DIV2 | 0x01;
    lh001_91_write_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_BUFCON, &bufcon_val, 1);

    // 6. 配置主从芯片的 ADC 通道 (ADCCHCON=0x10)
    lh001_91_adc_channel_dual(LH001_91_CHIP_MASTER, ENUM_LH001_91_ADCCHP_AIN0, ENUM_LH001_91_ADCCHN_AIN1);
    lh001_91_adc_channel_dual(LH001_91_CHIP_SLAVE, ENUM_LH001_91_ADCCHP_AIN0, ENUM_LH001_91_ADCCHN_AIN1);
	
    // 7. 显式修改主从器件的 LOCON3=0x00
    uint8_t locon3_val = 0x00;
    lh001_91_write_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_LOCON3, &locon3_val, 1);
    lh001_91_write_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_LOCON3, &locon3_val, 1);

    // 读取寄存器Dump数组进行校验打印
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_CONFIG1, &master_regs[0], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_LOCON1, &master_regs[1], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_LOCON2, &master_regs[2], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_LOCON3, &master_regs[3], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_LOFFSTAT, &master_regs[4], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_RLDCON, &master_regs[5], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_BUFCON, &master_regs[6], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_ADCCHCON, &master_regs[7], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_ADCCTRL, &master_regs[8], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_PGAGAIN, &master_regs[9], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_PGACTRL, &master_regs[10], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_PGACTRL1, &master_regs[11], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_SYSCLK, &master_regs[12], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_MODCLK, &master_regs[13], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_CLKCON, &master_regs[14], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_SPICTRL, &master_regs[15], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_INTOEN, &master_regs[16], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_MASTER, ADDR_LH001_91_FIFOSTA, &master_regs[17], 1);

    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_CONFIG1, &slave_regs[0], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_LOCON1, &slave_regs[1], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_LOCON2, &slave_regs[2], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_LOCON3, &slave_regs[3], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_LOFFSTAT, &slave_regs[4], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_RLDCON, &slave_regs[5], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_BUFCON, &slave_regs[6], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_ADCCHCON, &slave_regs[7], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_ADCCTRL, &slave_regs[8], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_PGAGAIN, &slave_regs[9], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_PGACTRL, &slave_regs[10], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_PGACTRL1, &slave_regs[11], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_SYSCLK, &slave_regs[12], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_MODCLK, &slave_regs[13], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_CLKCON, &slave_regs[14], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_SPICTRL, &slave_regs[15], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_INTOEN, &slave_regs[16], 1);
    lh001_91_read_regs_dual(LH001_91_CHIP_SLAVE, ADDR_LH001_91_FIFOSTA, &slave_regs[17], 1);

    printf("--- LH001_91 Reg Dump (Master vs Slave) ---\r\n");
    printf("CONFIG1:  M=0x%02X, S=0x%02X\r\n", master_regs[0], slave_regs[0]);
    printf("LOCON1:   M=0x%02X, S=0x%02X\r\n", master_regs[1], slave_regs[1]);
    printf("LOCON2:   M=0x%02X, S=0x%02X\r\n", master_regs[2], slave_regs[2]);
    printf("LOCON3:   M=0x%02X, S=0x%02X\r\n", master_regs[3], slave_regs[3]);
    printf("LOFFSTAT: M=0x%02X, S=0x%02X\r\n", master_regs[4], slave_regs[4]);
    printf("RLDCON:   M=0x%02X, S=0x%02X\r\n", master_regs[5], slave_regs[5]);
    printf("BUFCON:   M=0x%02X, S=0x%02X\r\n", master_regs[6], slave_regs[6]);
    printf("ADCCHCON: M=0x%02X, S=0x%02X\r\n", master_regs[7], slave_regs[7]);
    printf("ADCCTRL:  M=0x%02X, S=0x%02X\r\n", master_regs[8], slave_regs[8]);
    printf("PGAGAIN:  M=0x%02X, S=0x%02X\r\n", master_regs[9], slave_regs[9]);
    printf("PGACTRL:  M=0x%02X, S=0x%02X\r\n", master_regs[10], slave_regs[10]);
    printf("PGACTRL1: M=0x%02X, S=0x%02X\r\n", master_regs[11], slave_regs[11]);
    printf("SYSCLK:   M=0x%02X, S=0x%02X\r\n", master_regs[12], slave_regs[12]);
    printf("MODCLK:   M=0x%02X, S=0x%02X\r\n", master_regs[13], slave_regs[13]);
    printf("CLKCON:   M=0x%02X, S=0x%02X\r\n", master_regs[14], slave_regs[14]);
    printf("SPICTRL:  M=0x%02X, S=0x%02X\r\n", master_regs[15], slave_regs[15]);
    printf("INTOEN:   M=0x%02X, S=0x%02X\r\n", master_regs[16], slave_regs[16]);
    printf("FIFOSTA:  M=0x%02X, S=0x%02X\r\n", master_regs[17], slave_regs[17]);
    printf("-------------------------------------------\r\n");

    lh001_91_fifo_setup_dual(LH001_91_CHIP_MASTER, LH001_91_FIFO_THRESHOLD);
    lh001_91_fifo_setup_dual(LH001_91_CHIP_SLAVE, LH001_91_FIFO_THRESHOLD);

    printf("fifo mode start, threshold=%u\r\n", LH001_91_FIFO_THRESHOLD);

    lh001_91_adc_go_dual(LH001_91_CHIP_MASTER);
    lh001_91_adc_go_dual(LH001_91_CHIP_SLAVE);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SPI1_Init();
  MX_USART2_UART_Init();
  MX_TIM17_Init();
  MX_TIM6_Init();
  /* USER CODE BEGIN 2 */
  if (ecg_stream_init(&ecg_stream, &huart2, &htim6) != ECG_STREAM_OK)
  {
    Error_Handler();
  }
  LH001_91_App_Init();
  HAL_TIM_Base_Start_IT(&htim6); // 开启 1ms 匀速消费定时器中断
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // ---------------------------------------------------------
    // 生产者：DRDY 突发到达后读取主从 FIFO，交叉重组数据存入环形缓冲器中。
    // 消费者：TIM6 1ms 定时器中断自动匀速弹出 1 个 Slot 发送并微调重装载值。
    // ---------------------------------------------------------
    if (drdy_take_pending())
    {
        fifo_service_once();
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 5;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void LH001_91_DELAY_INIT(void)
{
    uint32_t pclk2_freq = HAL_RCC_GetPCLK2Freq(); 
    uint32_t tim17_clk_freq = pclk2_freq; 
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) != RCC_D2CFGR_D2PPRE2_DIV1)
    {
        tim17_clk_freq = pclk2_freq * 2;
    }
    uint32_t psc_value = (tim17_clk_freq / 1000000) - 1;

    __HAL_TIM_SET_PRESCALER(&htim17, psc_value);
    htim17.Instance->EGR = TIM_EGR_UG; 
    __HAL_TIM_CLEAR_FLAG(&htim17, TIM_FLAG_UPDATE); 
}

void UART_SendString(const char* str)
{
    HAL_UART_Transmit(&huart2, (uint8_t*)str, strlen(str), 1000);
}

/* 偏离说明: HAL 回调签名由 HAL 固定, 无法返回状态。此处只做入队与泵送, 不做判定。 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == DRDY1_Pin)
    {
        drdy1_irq_count++;

        if (fifo_irq_pending < DRDY_PENDING_MAX) {
            fifo_irq_pending++;
        } else {
            fifo_reentry_drop_count++;
        }
    }
}

/* 偏离说明: HAL 回调签名由 HAL 固定, 无法返回状态。
 * 发送完成后立即续发下一帧, 使输出节奏跟随采集节奏, 不受 SysTick 与芯片时钟的偏差影响。 */
/* 1ms 定时器中断回调: 匀速消费 Slot 环形缓冲，弹出 1 个 Slot 发送 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6)
    {
        (void)ecg_stream_pump_slot(&ecg_stream);
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        (void)ecg_stream_notify_tx_done(&ecg_stream);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        (void)ecg_stream_notify_tx_error(&ecg_stream);
    }
}

/* 偏离说明: 签名由 ARM C++ ABI 固定, 既不能返回状态也不能返回。
 * 工程启用 MicroLIB (useUlib=1), 而 MicroLIB 不提供 __aeabi_assert,
 * 故由应用层提供: 记录现场后停机, 便于调试器定位失败的断言。
 * 发布版应定义 NDEBUG, 届时 assert 全部编译掉, 本函数不再被引用。 */
void __aeabi_assert(const char *expr, const char *file, int line)
{
    assert_fail_expr = expr;
    assert_fail_file = file;
    assert_fail_line = line;
    Error_Handler();

    /* 故意不终止的循环: __aeabi_assert 声明为 noreturn, 而 Error_Handler
     * 未标注 noreturn, 需在此保证控制流确实不返回。 */
    while (1)
    {
    }
}

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
