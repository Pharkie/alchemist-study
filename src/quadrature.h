// ============================================================
// Alchemist Study — software quadrature decoder (shared)
//
// The ESP32-C3 lacks the PCNT pulse-counter peripheral that the usual
// ESP32Encoder library needs, so the encoder is decoded in software: both
// channels interrupt on CHANGE and a 16-entry transition table maps each
// (prev,curr) 2-bit state to -1/0/+1. Invalid / no-change entries are 0,
// which debounces contact bounce for free. This is the equivalent of
// attachHalfQuad with internal pullups.
//
// Header-only on purpose: each program in this repo is a single
// translation unit (see build_src_filter in platformio.ini), so the
// `static` definitions here are simply that program's private copy.
// ============================================================
#pragma once

#include <Arduino.h>
#include "pins.h"

static volatile int32_t g_encoderCount = 0;  // running signed position
static volatile uint8_t s_quadPrev = 0;

// Index = (prev<<2)|curr. Valid single-step transitions map to +/-1.
static const int8_t kQuadTable[16] = {
  0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0
};

static void IRAM_ATTR encoderISR() {
  uint8_t curr = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  g_encoderCount += kQuadTable[(s_quadPrev << 2) | curr];
  s_quadPrev = curr;
}

// Pullups on both channels, seed the state, then decode on every edge.
static void encoderBegin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  s_quadPrev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderISR, CHANGE);
}
