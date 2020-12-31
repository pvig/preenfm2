/*
 * Copyright 2013 Xavier Hosxe
 *
 * Author: Xavier Hosxe (xavier <dot> hosxe (at) g m a i l <dot> com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "stm32f4xx_conf.h"
#include "RingBuffer.h"
#include "LiquidCrystal.h"
#include "usb_core.h"
#include "Synth.h"
#include "MidiDecoder.h"

#ifndef _PreenFM_H_
#define _PreenFM_H_


#ifndef CVIN

#define CVIN_STRING ""

#else

#define CVIN_STRING "_cv"
extern int TIM2PerSeq;
void ADC_Config(uint32_t adcBufferAdress);

#endif


#define LEDPIN GPIO_Pin_6


extern int spiState ;

extern LiquidCrystal lcd;
extern Synth synth;
extern SynthState         synthState;
extern MidiDecoder        midiDecoder;
extern RingBuffer<uint8_t, 200> usartBufferIn;
extern RingBuffer<uint8_t, 100> usartBufferOut;

extern USB_OTG_CORE_HANDLE  usbOTGHost;
extern USB_OTG_CORE_HANDLE  usbOTGDevice;
extern USBD_Usr_cb_TypeDef midiStreamingUsrCallback;


extern uint32_t cpt1 ;
extern uint32_t cpt2 ;
extern int state;
extern bool usbReady;
extern char usbHostText[128];
extern unsigned int preenTimer;

extern Synth synth;

enum PcbVersion {
    PCB_R5 = 0b11,
    PCB_R6_CS4344 = 0b01
};

void fillSoundBuffer();
void USART_Config();
void LEDFront_Config();
void LEDTest_Config(uint16_t pin);

uint8_t getPcbVersion();
void MCP4922_Config();
void MCP4922_screenBoot(Synth& synth);
void MCP4922_SysTick_Config();

void CS4344_Config(int32_t *sample);
void CS4344_Timer_Config();
void CS4344_screenBoot();

void RNG_Config();
void LCD_InitChars(LiquidCrystal* lcd);

void strobePin(u8 count, u32 rate);

#endif
