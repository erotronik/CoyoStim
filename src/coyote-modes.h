
// https://github.com/OpenDGLab/OpenDGLab-Connect/blob/master/src/services/DGLab.js

// For Coyote 2 these patterns updated every 100mS sending a single set of data
// but for Coyote 3 we can send 4 sets of data every 100mS which will get output
// every 25mS. So alter all the routines to expect to be called at 25mS and
// if the device is a coyote2 we will skip some

// So now amplitude is 0-100 (for coyote2 is mapped to 0-20)

#pragma once

#include "coyote.hpp"
#include <math.h>
#include <cmath>

coyote_pattern coyote_mode_nothing(uint32_t &waveclock, uint32_t &cyclecount) {
  return {};
}

coyote_pattern coyote_mode_breath(uint32_t &waveclock, uint32_t &cyclecount) {
  // like the 'breathe' mode (1100mS cycle)
  coyote_pattern out;
  //instead of settting up an array like this, let's write it as a formula
  //Int8Array(3) [33, 1, 0]  0: 1 9 0
  //Int8Array(3) [33, 1, 2]  1: 1 9 4
  //Int8Array(3) [33, 1, 4]  2: 1 9 8
  //Int8Array(3) [33, 1, 6]  3: 1 9 12
  //Int8Array(3) [33, 1, 8]  4: 1 9 16
  //Int8Array(3) [33, 1, 10] 5: 1 9 20
  //Int8Array(3) [33, 1, 10] 6: 1 9 20
  //Int8Array(3) [33, 1, 10] 7: 1 9 20
  //Int8Array(3) [0, 0, 0]   8: 0 0 0
  //Int8Array(3) [0, 0, 0]   9: 0 0 0
  //Int8Array(3) [0, 0, 0]  10: 0 0 0
  if (waveclock < 8*4) {
    out.pulse_length = 1;
    out.pause_length = 9;
    out.frequency = 10; // could work this out from pulse/pause also but...
    out.amplitude = waveclock*4;
    if (out.amplitude > 100) out.amplitude = 100;
  }
  (waveclock)++;
  if (waveclock > (7+3)*4) {
    waveclock = 0;
    //cyclecount--;
  }
  return out;
}

coyote_pattern coyote_mode_waves(uint32_t &waveclock, uint32_t &cyclecount) {
  coyote_pattern out;

  constexpr uint16_t rampUpTime = 30*4;
  constexpr uint16_t rampDownTime = 50*4;
  constexpr uint16_t cycleTime = rampUpTime + rampDownTime;
  constexpr uint16_t maxAmp = 100;
  // double piOverTwo = std::atan(1) * 2; // incorrectly not marked constexpr in stdlib
  constexpr double piOverTwo = M_PI_2;

  out.pulse_length = 10;

  if ( waveclock <= rampUpTime ) {
    // Sin wave goes from 0 -> 1 over the values 0 -> pi/2
    auto index = (double)waveclock / (double)rampUpTime;
    out.amplitude = std::floor(std::sin((piOverTwo * index)) * (double)maxAmp);
    //Serial.printf("1: waveclock: %u, cyclecount: %u, pause_length: %u, amplitude: %u\n", waveclock, cyclecount, out.pause_length, out.amplitude);
  } else {
    // Sin wave goes from 1 -> 0 over the values pi/2 -> pi
    auto index = (double)(waveclock - rampUpTime) / (double)rampDownTime;
    out.amplitude = std::floor(std::sin((piOverTwo * index) + piOverTwo) * (double)maxAmp);
    //Serial.printf("2: waveclock: %u, cyclecount: %u, pause_length: %u, amplitude: %u\n", waveclock, cyclecount, out.pause_length, out.amplitude);
  }

  // Longer pause length creates more intense feelings
  // so make this longer to a peak and then cycle back.
  out.pause_length = 10 * ((cyclecount % 8) + 2); // so 20 to 90
  out.frequency = 10 + cyclecount%8 * 3; // for coyote3 go from 10 to 30 over say 8

  waveclock++;
  if ( waveclock > cycleTime ) {
    waveclock = 0;
    cyclecount++;
  }
  return out;
}
