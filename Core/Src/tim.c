/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    tim.c
  * @brief   This file provides code for the configuration
  *          of the TIM instances.
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
#include "tim.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

TIM_HandleTypeDef htim17;
TIM_HandleTypeDef htim6;

/* TIM17 init function */
void MX_TIM17_Init(void)
{
  htim17.Instance = TIM17;
  htim17.Init.Prescaler = 0;
  htim17.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim17.Init.Period = 65535;
  htim17.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim17.Init.RepetitionCounter = 0;
  htim17.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim17) != HAL_OK)
  {
    Error_Handler();
  }
}

/* TIM6 init function for 1ms stream consumer */
void MX_TIM6_Init(void)
{
  htim6.Instance = TIM6;
  /* TIM6 输入时钟为 240MHz, Prescaler = 239 => 1MHz (1us/tick) */
  htim6.Init.Prescaler = 239;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  /* Period = 999 => 1000us (1ms) 初始周期 */
  htim6.Init.Period = 999;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef* tim_baseHandle)
{
  if(tim_baseHandle->Instance==TIM17)
  {
    __HAL_RCC_TIM17_CLK_ENABLE();
  }
  else if(tim_baseHandle->Instance==TIM6)
  {
    __HAL_RCC_TIM6_CLK_ENABLE();
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
  }
}

void HAL_TIM_Base_MspDeInit(TIM_HandleTypeDef* tim_baseHandle)
{
  if(tim_baseHandle->Instance==TIM17)
  {
    __HAL_RCC_TIM17_CLK_DISABLE();
  }
  else if(tim_baseHandle->Instance==TIM6)
  {
    __HAL_RCC_TIM6_CLK_DISABLE();
    HAL_NVIC_DisableIRQ(TIM6_DAC_IRQn);
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
