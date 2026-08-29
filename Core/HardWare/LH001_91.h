/**
  ******************************************************************************
  * @file           : LH001_91.h
  * @brief          : LH001-91 ECG Chip Driver Header File (Dual-Chip Master/Slave)
  * @author         : LH Software Group
  ******************************************************************************
  */

#ifndef __LH001_91_H
#define __LH001_91_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include "lh001_91_port.h"

/* Hardware & Reference Constants --------------------------------------------*/
#define ADC_CONV_START_HW                   1
#define REG_VALUE_TBD                       0x00
#define ADC_REF_MV                          2510

/*******************************************************************************
 * Register Address Definitions
 ******************************************************************************/
#define ADDR_LH001_91_SYSCLK                0x14
#define ADDR_LH001_91_MODCLK                0x15
#define ADDR_LH001_91_CLKCON                0x16
#define ADDR_LH001_91_SPICTRL               0x17
#define ADDR_LH001_91_INTOEN                0x18
#define ADDR_LH001_91_FIFOSTA               0x19
#define ADDR_LH001_91_LOACCLKSEL            0x1A
#define ADDR_LH001_91_OUTCLK                0x1C
#define ADDR_LH001_91_USERKEY               0x1E
#define ADDR_LH001_91_CHIPID                0x40

#define ADDR_LH001_91_CONFIG1               0x01
#define ADDR_LH001_91_ADCCHOP               0x4B
#define ADDR_LH001_91_ADCCTRL               0x0C
#define ADDR_LH001_91_ADCCHCON              0x0B
#define ADDR_LH001_91_BUFCON                0x0A

#define ADDR_LH001_91_PGAGAIN               0x0D
#define ADDR_LH001_91_PGACTRL               0x0E
#define ADDR_LH001_91_PGACTRL1              0x0F

#define ADDR_LH001_91_LOCON1                0x03
#define ADDR_LH001_91_LOCON2                0x04
#define ADDR_LH001_91_LOCON3                0x05
#define ADDR_LH001_91_LOFFSTAT              0x06
#define ADDR_LH001_91_RLDCON                0x07

#define ADDR_LH001_91_PLTFMPD               0x55
#define ADDR_LH001_91_MODCFG                0x64

/*******************************************************************************
 * Register Bit Mask and Position Definitions
 ******************************************************************************/
#define MSK_LH001_91_MODCLK_CD              0xFF
#define POS_LH001_91_MODCLK_CD              0x00

#define MSK_LH001_91_CLKCON_CLKEN           0x01
#define POS_LH001_91_CLKCON_CLKEN           0
#define MSK_LH001_91_CLKCON_F2M             0x02
#define POS_LH001_91_CLKCON_F2M             1
#define MSK_LH001_91_CLKCON_PU              0x04
#define POS_LH001_91_CLKCON_PU              2

#define MSK_LH001_91_SPICTRL_FIFOEN         0x02
#define POS_LH001_91_SPICTRL_FIFOEN         1

#define MSK_LH001_91_INTOEN_INTEN           0x0F
#define POS_LH001_91_INTOEN_INTEN           0
#define MSK_LH001_91_INTOEN_ENOUGH          (0x01 << 1)

#define MSK_LH001_91_FIFOSTA_ENOUGHCNT      (0x1F << 0)
#define POS_LH001_91_FIFOSTA_ENOUGHCNT      (0)
#define LH001_91_FIFO_MAX_DEPTH             32U

/* ADC */
#define MSK_LH001_91_ADCCTRL_ADCEN          (0x01 << 0)
#define POS_LH001_91_ADCCTRL_ADCEN          (0)
#define MSK_LH001_91_ADCCTRL_AMPEN          (0x01 << 1)
#define POS_LH001_91_ADCCTRL_AMPEN          (1)

#define MSK_LH001_91_CONFIG1_DR             (0x07 << 0)
#define MSK_LH001_91_CONFIG1_SGLESHOT       (0x01 << 3)
#define POS_LH001_91_CONFIG1_DR             (0)
#define POS_LH001_91_CONFIG1_SGLESHOT       (3)

#define POS_LH001_91_BUFCON_AVDD_DIV2       (2)
#define MSK_LH001_91_BUFCON_AVDD_DIV2       (1 << POS_LH001_91_BUFCON_AVDD_DIV2)
#define POS_LH001_91_BUFCON_ENREF           (4)
#define MSK_LH001_91_BUFCON_ENREF           (1 << POS_LH001_91_BUFCON_ENREF)
#define POS_LH001_91_BUFCON_EN2P5           (5)
#define MSK_LH001_91_BUFCON_EN2P5           (0x01 << POS_LH001_91_BUFCON_EN2P5)

#define POS_LH001_91_ADCCHCON_PSEL          (0)
#define POS_LH001_91_ADCCHCON_NSEL          (4)

/* PGA */
#define POS_LH001_91_PGACTRL_PD             (0)
#define MSK_LH001_91_PGACTRL_PD             (1 << POS_LH001_91_PGACTRL_PD)
#define POS_LH001_91_PGACTRL_BYPASS         (1)
#define MSK_LH001_91_PGACTRL_BYPASS         (1 << POS_LH001_91_PGACTRL_BYPASS)
#define POS_LH001_91_PGACTRL_LP             (3)
#define MSK_LH001_91_PGACTRL_LP             (1 << POS_LH001_91_PGACTRL_LP)

#define POS_LH001_91_PGACTRL1_CHOP          (0)
#define MSK_LH001_91_PGACTRL1_CHOP          (3 << POS_LH001_91_PGACTRL1_CHOP)
#define POS_LH001_91_PGACTRL1_CMSNS         (2)
#define MSK_LH001_91_PGACTRL1_CMSNS         (1 << POS_LH001_91_PGACTRL1_CMSNS)

#define POS_LH001_91_PGAGAIN                (0)
#define MSK_LH001_91_PGAGAIN                (0x3F << POS_LH001_91_PGAGAIN)

/* ECG */
#define POS_LH001_91_LOCON1_THRESHOLD       (0)
#define POS_LH001_91_LOCON1_CURRENT         (4)
#define POS_LH001_91_LOCON2_SINKEN          (1)
#define MSK_LH001_91_LOCON2_SINKEN          (3 << POS_LH001_91_LOCON2_SINKEN)
#define POS_LH001_91_LOCON2_SOURCEENB       (3)
#define MSK_LH001_91_LOCON2_SOURCEENB       (3 << POS_LH001_91_LOCON2_SOURCEENB)
#define POS_LH001_91_LOCON2_RLDLOEN         (5)
#define MSK_LH001_91_LOCON2_RLDLOEN         (1 << POS_LH001_91_LOCON2_RLDLOEN)

#define POS_LH001_91_LOCON3_DCLOEN          (0)
#define MSK_LH001_91_LOCON3_DCLOEN          (7 << POS_LH001_91_LOCON3_DCLOEN)
#define POS_LH001_91_LOCON3_DCLOEN0         (0)
#define POS_LH001_91_LOCON3_DCLOEN1         (1)
#define POS_LH001_91_LOCON3_DCLOEN2         (2)

#define POS_LH001_91_RLDCON_CHOPFREQ        (0)
#define POS_LH001_91_RLDCON_EN              (2)
#define POS_LH001_91_RLDCON_RLDREFSEL       (3)

/*******************************************************************************
 * Enumeration Definitions
 ******************************************************************************/
#define ENUM_LH001_91_ADC_SINGLE            0x01
#define ENUM_LH001_91_ADC_CONTINOUS         0x00
#define ENUM_LH001_91_ADC_REF_2V            0x00
#define ENUM_LH001_91_ADC_REF_2V5           0x01

#define ENUM_LH001_91_ADCCHP_AIN0           0x00
#define ENUM_LH001_91_ADCCHP_AIN1           0x01
#define ENUM_LH001_91_ADCCHN_AIN0           0x00
#define ENUM_LH001_91_ADCCHN_AIN1           0x01

#define ENUM_LH001_91_PGA_PGAGAIN_6         0x04
#define ENUM_LH001_91_PGA_POWER_LP          0x01
#define ENUM_LH001_91_PGA_CHOP_DIV16        2

#define LH001_91_USERKEY                    0x86
#define ENUM_LH001_91_RLD_CHOP_CLOSE        0
#define ENUM_LH001_91_RLD_CHOP_FMOD_DIV32   1
#define ENUM_LH001_91_RLD_REF_AVDD_DIV2     0

/*******************************************************************************
 * SPI Command Opcodes
 ******************************************************************************/
#define SPI_CMD_RESET                       0x06
#define SPI_CMD_CONV_START                  0x08
#define SPI_CMD_CONV_STOP                   0x0A
#define SPI_CMD_RST_FIFO                    0x0C
#define SPI_CMD_RDATAC                      0x10
#define SPI_CMD_SDATAC                      0x11
#define SPI_CMD_RDATA                       0x12
#define SPI_CMD_RREG_MSK                    0x20
#define SPI_CMD_WREG_MSK                    0x40
#define SPI_CMD_RREG_BK1_MSK                0xA0
#define SPI_CMD_WREG_BK1_MSK                0xC0

/*******************************************************************************
 * Configuration Structure Constants
 ******************************************************************************/
#define ENUM_LH001_91_ADC_FMODCLK_128K      16
#define ENUM_LH001_91_ADC_DR_DIV128         3

#define LH001_91_LEADOFF_DELTA_4            3
#define LH001_91_LEADOFF_CURRENT_SOURCE_50nA 3
#define LH001_91_LEADOFF_SINK_CLOSE         0
#define LH001_91_LEADOFF_SOURCE_CLOSE       3

/*******************************************************************************
 * Type Definitions & Structures
 ******************************************************************************/
typedef struct {
    uint32_t Fmodclk;
    uint32_t oversample_rate;
    uint32_t conv_mode;
    uint32_t ref_sel;
} LH001_91_ADC_INIT_t;

typedef struct {
    uint32_t gain;
    uint32_t pga_bypass;
    uint32_t pga_chop;
    uint32_t power_mode;
    uint32_t comm_sense_en;
} LH001_91_PGA_INIT_t;

typedef struct {
    uint32_t threshold;
    uint32_t current;
    uint32_t sink_cfg;
    uint32_t source_cfg;
    uint32_t rld_sink_en;
    uint32_t ain0_detect_en;
    uint32_t ain1_detect_en;
    uint32_t rld_detect_en; 
} LH001_91_LEADOFF_t;

typedef enum {
    LH001_91_CHIP_MASTER = 0,  // 选择主芯片 (CS1 -> PA4)
    LH001_91_CHIP_SLAVE  = 1   // 选择从芯片 (CS2 -> PA1)
} LH001_91_Chip_ID_t;

typedef struct {
    volatile uint64_t reserved0 : 48;
    volatile uint64_t start     : 4;
    volatile uint64_t loffstat  : 6;
    volatile uint64_t gpio      : 2;
    volatile uint64_t reserved1 : 10;
    volatile uint64_t data      : 24;
    volatile uint64_t tail      : 2;
} ADC_DATA_FIFO_t;

typedef enum {
    LH001_91_FIFO_READ_OK = 0,
    LH001_91_FIFO_READ_OVERFLOW,
    LH001_91_FIFO_READ_EMPTY,
    LH001_91_FIFO_READ_NO_TAIL,
    LH001_91_FIFO_READ_BAD_ARG,
} LH001_91_FIFO_READ_STATUS_t;

typedef struct {
    ADC_DATA_FIFO_t frames[LH001_91_FIFO_MAX_DEPTH];
    uint8_t data_buf[LH001_91_FIFO_MAX_DEPTH * 6];
    uint8_t count;
    uint8_t last_tail;
    LH001_91_FIFO_READ_STATUS_t status;
} LH001_91_FIFO_BURST_t;

/*******************************************************************************
 * Clean API Declarations
 ******************************************************************************/
void lh001_91_hw_reset(void);
void lh001_91_adc_go_dual(LH001_91_Chip_ID_t chip_id);
void lh001_91_adc_stop_dual(LH001_91_Chip_ID_t chip_id);

void lh001_91_reset_fifo_hw(void);
void lh001_91_reset_fifo_cmd_dual(LH001_91_Chip_ID_t chip_id);
void lh001_91_reset_fifo_cmd(void);

void lh001_91_write_regs_dual(LH001_91_Chip_ID_t chip_id, uint8_t addr, uint8_t *pRegs, uint8_t num);
void lh001_91_read_regs_dual(LH001_91_Chip_ID_t chip_id, uint8_t addr, uint8_t *pRegs, uint8_t num);

void lh001_91_fifo_setup_dual(LH001_91_Chip_ID_t chip_id, uint8_t thresh_num);
void lh001_91_read_fifo_burst_dual_num(LH001_91_Chip_ID_t chip_id, LH001_91_FIFO_BURST_t *pBurst, uint8_t expected_count);

void lh001_91_adc_init_dual(LH001_91_Chip_ID_t chip_id, LH001_91_ADC_INIT_t *pInit);
void lh001_91_pga_init_dual(LH001_91_Chip_ID_t chip_id, LH001_91_PGA_INIT_t *pInit);
void lh001_91_leadoff_init_dual(LH001_91_Chip_ID_t chip_id, LH001_91_LEADOFF_t *pInit);
void lh001_91_rld_init_dual(LH001_91_Chip_ID_t chip_id, uint32_t enable, uint32_t refsel, uint32_t chop);
void lh001_91_adc_channel_dual(LH001_91_Chip_ID_t chip_id, uint32_t chP, uint32_t chN);

float lh001_91_adc_code2mv(uint32_t code, uint32_t ref);

#ifdef __cplusplus
}
#endif

#endif  /* __LH001_91_H */
