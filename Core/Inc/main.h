/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define SPI_CS2_Pin GPIO_PIN_1
#define SPI_CS2_GPIO_Port GPIOA
#define SPI_CS1_Pin GPIO_PIN_4
#define SPI_CS1_GPIO_Port GPIOA
#define Start_Pin GPIO_PIN_0
#define Start_GPIO_Port GPIOB
#define INT2_Pin GPIO_PIN_1
#define INT2_GPIO_Port GPIOB
#define INT2_EXTI_IRQn EXTI1_IRQn
#define INT1_Pin GPIO_PIN_2
#define INT1_GPIO_Port GPIOB
#define INT1_EXTI_IRQn EXTI2_IRQn
#define RA_PULL_Pin GPIO_PIN_10
#define RA_PULL_GPIO_Port GPIOB
#define DRDY2_Pin GPIO_PIN_14
#define DRDY2_GPIO_Port GPIOB
#define DRDY2_EXTI_IRQn EXTI15_10_IRQn
#define DRDY1_Pin GPIO_PIN_15
#define DRDY1_GPIO_Port GPIOB
#define DRDY1_EXTI_IRQn EXTI15_10_IRQn
#define LED_Pin GPIO_PIN_15
#define LED_GPIO_Port GPIOA
#define LL_PULL_Pin GPIO_PIN_7
#define LL_PULL_GPIO_Port GPIOB
#define ECG_Rset_Pin GPIO_PIN_8
#define ECG_Rset_GPIO_Port GPIOB
#define LA_PULL_Pin GPIO_PIN_9
#define LA_PULL_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
