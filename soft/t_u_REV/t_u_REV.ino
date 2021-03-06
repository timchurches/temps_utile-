// Copyright (c) 2015, 2016 Max Stadler, Patrick Dowling
//
// Original Author : Max Stadler
// Heavily modified: Patrick Dowling (pld@gurkenkiste.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and asscoiated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Main startup/loop for T_U firmware; adapted from o_C

#include <EEPROM.h>

#include "TU_apps.h"
#include "TU_core.h"
#include "TU_outputs.h"
#include "TU_debug.h"
#include "TU_gpio.h"
#include "TU_ADC.h"
#include "TU_calibration.h"
#include "TU_digital_inputs.h"
#include "TU_menus.h"
#include "TU_ui.h"
#include "TU_version.h"
#include "TU_options.h"
#include "src/display.h"
#include "src/ADC/OC_util_ADC.h"
#include "util/util_debugpins.h"
#include "TU_BPM.h"

unsigned long LAST_REDRAW_TIME = 0;
uint_fast8_t MENU_REDRAW = true;
TU::UiMode ui_mode = TU::UI_MODE_MENU;

/*  --------------------- UI timer ISR -------------------------   */

IntervalTimer UI_timer;

void FASTRUN UI_timer_ISR() {
  TU_DEBUG_PROFILE_SCOPE(TU::DEBUG::UI_cycles);
  TU::ui.Poll();

  TU_DEBUG_RESET_CYCLES(TU::ui.ticks(), 2048, TU::DEBUG::UI_cycles);
}

/*  ------------------------ core timer ISR ---------------------------   */
IntervalTimer CORE_timer;
volatile bool TU::CORE::app_isr_enabled = false;
volatile uint32_t TU::CORE::ticks = 0;

void FASTRUN CORE_timer_ISR() {
  DEBUG_PIN_SCOPE(DEBUG_PIN_2);
  TU_DEBUG_PROFILE_SCOPE(TU::DEBUG::ISR_cycles);

  // display uses SPI. By first updating the clock values, then starting
  // a DMA transfer to the display things are fairly nicely interleaved. In the
  // next ISR, the display transfer is finalized (CS update).

  display::Flush();
  TU::OUTPUTS::Update();
  display::Update();

  // The ADC scan uses async startSingleRead/readSingle and single channel each
  // loop, so should be fast enough even at 60us (check ADC::busy_waits() == 0)
  // to verify. Effectively, the scan rate is ISR / 4 / ADC::kAdcSmoothing
  // 100us: 10kHz / 4 / 4 ~ .6kHz
  // 60us: 16.666K / 4 / 4 ~ 1kHz
  // kAdcSmoothing == 4 has some (maybe 1-2LSB) jitter but seems "Good Enough".
  TU::ADC::Scan();
  // Pin changes are tracked in separate ISRs, so depending on prio it might
  // need extra precautions.
  TU::DigitalInputs::Scan();

#ifndef TU_UI_SEPARATE_ISR
  TODO needs a counter
  UI_timer_ISR();
#endif

  ++TU::CORE::ticks;
  if (TU::CORE::app_isr_enabled)
    TU::apps::ISR();

  TU_DEBUG_RESET_CYCLES(TU::CORE::ticks, 16384, TU::DEBUG::ISR_cycles);
}

/*       ---------------------------------------------------------         */

void setup() {
 
  delay(100);
  NVIC_SET_PRIORITY(IRQ_PORTB, 0); // TR1 = 0 = PTB16
  TU::OUTPUTS::SPI_Init();
  SERIAL_PRINTLN("* t_u BOOTING...");
  SERIAL_PRINTLN("* %s", TU_VERSION);
  TU::DEBUG::Init();
  TU::DigitalInputs::Init();
  delay(500);
  TU::ADC::Init(&TU::calibration_data.adc); // Yes, it's using the calibration_data before it's loaded...
  TU::OUTPUTS::Init(&TU::calibration_data.dac);
   
  display::Init();

  GRAPHICS_BEGIN_FRAME(true);
  GRAPHICS_END_FRAME();

  calibration_load();
  display::AdjustOffset(TU::calibration_data.display_offset);

  TU::menu::Init();
  TU::ui.Init();
  TU::ui.configure_encoders(TU::calibration_data.encoder_config());

  SERIAL_PRINTLN("* Starting CORE ISR @%luus", TU_CORE_TIMER_RATE);
  CORE_timer.begin(CORE_timer_ISR, TU_CORE_TIMER_RATE);
  CORE_timer.priority(TU_CORE_TIMER_PRIO);

#ifdef TU_UI_SEPARATE_ISR
  SERIAL_PRINTLN("* Starting UI ISR @%luus", TU_UI_TIMER_RATE);
  UI_timer.begin(UI_timer_ISR, TU_UI_TIMER_RATE);
  UI_timer.priority(TU_UI_TIMER_PRIO);
#endif

  // Display splash screen and optional calibration
  bool reset_settings = false;
  ui_mode = TU::ui.Splashscreen(reset_settings);
  
  if (ui_mode == TU::UI_MODE_CALIBRATE) {
    TU::ui.Calibrate();
    ui_mode = TU::UI_MODE_MENU;
  }
  // set approx. v/oct value (from calibration data)
  TU::OUTPUTS::set_v_oct();
  // initialize apps
  TU::apps::Init(reset_settings);
}

/*  ---------    main loop  --------  */

void FASTRUN loop() {

  TU::CORE::app_isr_enabled = true;
  uint32_t menu_redraws = 0;
  while (true) {

    // don't change current_app while it's running
    if (TU::UI_MODE_APP_SETTINGS == ui_mode) {
      TU::ui.AppSettings();
      ui_mode = TU::UI_MODE_MENU;
    }

    // Refresh display
    if (MENU_REDRAW) {
      GRAPHICS_BEGIN_FRAME(false); // Don't busy wait
        if (TU::UI_MODE_MENU == ui_mode) {
          TU_DEBUG_RESET_CYCLES(menu_redraws, 512, TU::DEBUG::MENU_draw_cycles);
          TU_DEBUG_PROFILE_SCOPE(TU::DEBUG::MENU_draw_cycles);
          TU::apps::current_app->DrawMenu();
          ++menu_redraws;
        } else {
          TU::apps::current_app->DrawScreensaver();
        }
        MENU_REDRAW = 0;
        LAST_REDRAW_TIME = millis();
      GRAPHICS_END_FRAME();
    }

    // Run current app
    TU::apps::current_app->loop();

    // UI events
    TU::UiMode mode = TU::ui.DispatchEvents(TU::apps::current_app);

    // State transition for app
    if (mode != ui_mode) {
      if (TU::UI_MODE_SCREENSAVER == mode)
        TU::apps::current_app->HandleAppEvent(TU::APP_EVENT_SCREENSAVER_ON);
      else if (TU::UI_MODE_SCREENSAVER == ui_mode)
        TU::apps::current_app->HandleAppEvent(TU::APP_EVENT_SCREENSAVER_OFF);
      ui_mode = mode;
    }

    if (millis() - LAST_REDRAW_TIME > REDRAW_TIMEOUT_MS)
      MENU_REDRAW = 1;
  }
}
