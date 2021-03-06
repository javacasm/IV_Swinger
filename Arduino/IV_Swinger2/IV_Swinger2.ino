/*
 *-----------------------------------------------------------------------------
 *
 * IV_Swinger2.ino: IV Swinger 2 Arduino sketch
 *
 * Copyright (C) 2017  Chris Satterlee
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *-----------------------------------------------------------------------------
 *
 * IV Swinger and IV Swinger 2 are open source hardware and software
 * projects
 *
 * Permission to use the hardware designs is granted under the terms of
 * the TAPR Open Hardware License Version 1.0 (May 25, 2007) -
 * http://www.tapr.org/OHL
 *
 * Permission to use the software is granted under the terms of the GNU
 * GPL v3 as noted above.
 *
 * Current versions of the licensing files, documentation, Fritzing file
 * (hardware description), and software can be found at:
 *
 *    https://github.com/csatt/IV_Swinger
 *
 *-----------------------------------------------------------------------------
 *
 * This file contains the Arduino sketch for the IV Swinger 2. It
 * performs the following functions:
 *
 *     - Participates in handshakes with the host computer (via USB)
 *     - Receives configuration options from the host
 *     - Communicates debug messages to the host
 *     - Controls the relay that switches the capacitor between the
 *       bleed circuit and the PV circuit
 *     - Reads and records values from the two ADC channels
 *     - Waits for current to stabilize at the beginning
 *     - Compensates for the fact that time passes between the current
 *       and voltage measurements
 *     - Selectively discards values so that the Arduino memory isn't
 *       exhausted before the IV curve is complete
 *     - Determines when the IV curve is complete
 *     - Sends results to the host
 *
 * Performance is important. The rate that the curve is "swung" is a
 * function of the capacitor value and the PV module; there is no way to
 * slow it down (other than using a larger capacitance).  The faster the
 * software can take measurements, the closer together the points will
 * be, which improves the "resolution" of the IV curve.  Because i = C *
 * dv/dt, the speed of the sweep is not constant from the Isc end of the
 * curve to the Voc end. It is faster (i.e. dt is smaller) when current
 * (i) is higher and when the voltage change (dv) between points is
 * lower. At the beginning of the curve, i is high, but dv is also high,
 * so the sweep speed is moderate. And at the end of the curve, both i
 * and dv are low, so the sweep speed is also moderate. But just past
 * the knee, i is still high but dv is low, so the sweep rate is the
 * highest. If the software performance is poor, this part of the curve
 * will have poor resolution.
 *
 * The downside of taking measurements quickly is that too many
 * measurements are taken during the parts of the curve where the sweep
 * rate is low. The Arduino has very limited memory, so if all these
 * points are recorded, memory will be exhausted before the curve is
 * complete. The software must selectively discard points to prevent
 * this from happening. The trick is to determine which points to
 * discard. It is not useful to have points that are very close to each
 * other, so the discard criterion is based on the distance between
 * points. This calculation has to be very fast because it is performed
 * after every measurement, and that reduces the rate that measurements
 * can be taken. Any use of floating point math, or even 32-bit (long)
 * math slows things down drastically, so only 16-bit integer math is
 * used. Instead of Pythagorean distance, so-called Manhattan distance
 * is used, which only requires subtraction and addition. The criterion
 * distance could be a constant, but that would not produce good results
 * for all IV curves. Instead, it is scaled based on the measured values
 * for Voc and Isc. The Voc values are read before the relay is
 * activated and the Isc values are determined just after the relay is
 * activated. The minimum distance criterion calculation requires some
 * computation time between the first two measured points, but that is
 * not normally a resolution-sensitive part of the curve. Nevertheless,
 * this code is also restricted to simple 16-bit integer math in order
 * to make it as quick as possible.
 *
 * A single point on the curve requires reading both channels of the
 * ADC. There is no way to read both values at the same time; each read
 * requires a separate SPI transaction, so some time passes between the
 * two reads, and the values do not represent the exact same point in
 * time. The simplest way to deal with this would be to ignore it; if
 * the points are close enough together, the effect is relatively
 * minor. But it isn't difficult to compensate for, so we do.  One way
 * to compensate would be to do three reads for each pair
 * (i.e. CH0/CH1/CH0 or CH1/CH0/CH1) and average the first and third.
 * But that would slow things down by 50%.  Instead, we just do one read
 * of each channel on each iteration, but interpolate between the CH1
 * values of each iteration. The catch is that there is computation
 * between iterations (which takes time), so it's not a simple average;
 * it's a weighted average based on measured times.
 *
 */
#include <SPI.h>

// Compile-time assertion macros (from Stack Overflow)
#define COMPILER_ASSERT(predicate) _impl_CASSERT_LINE(predicate,__LINE__)
#define _impl_PASTE(a,b) a##b
#define _impl_CASSERT_LINE(predicate, line) \
    typedef char _impl_PASTE(assertion_failed_on_line_,line)[2*!!(predicate)-1];

#define VERSION "1.0.1"        // Version of this Arduino sketch
#define FALSE 0                // Boolean false value
#define TRUE 1                 // Boolean true value
#define MAX_UINT (1<<16)-1     // Max unsigned integer
#define MAX_INT (1<<15)-1      // Max integer
#define MAX_ULONG (1<<32)-1    // Max unsigned long integer
#define MAX_LONG (1<<31)-1     // Max long integer
#define MAX_MSG_LEN 30         // Maximum length of a host message
#define MSG_TIMER_TIMEOUT 1000 // Number of times to poll for host message
#define CLK_DIV SPI_CLOCK_DIV8 // SPI clock divider ratio
#define SERIAL_BAUD 57600      // Serial port baud rate
#define ADC_MAX 4096.0         // Max count of ADC (2^^num_bits)
#define ADC_CS_PIN 10          // Arduino pin used for ADC chip select
#define RELAY_PIN 2            // Arduino pin used to activate relay
#define CS_INACTIVE HIGH       // Chip select is active low
#define CS_ACTIVE LOW          // Chip select is active low
#define VOLTAGE_CH 0           // ADC channel used for voltage measurement
#define CURRENT_CH 1           // ADC channel used for current measurement
#define RELAY_INACTIVE HIGH    // Relay pin is active low
#define RELAY_ACTIVE LOW       // Relay pin is active low
#define MAX_IV_POINTS 275      // Max number of I/V pairs to capture
#define MAX_IV_MEAS 60000      // Max number of I/V measurements (inc discards)
#define CH1_1ST_WEIGHT 5       // Amount to weigh 1st CH1 value in avg calc
#define CH1_2ND_WEIGHT 3       // Amount to weigh 2nd CH1 value in avg calc
#define MIN_ISC_ADC 100        // Minimum ADC count for Isc
#define MAX_ISC_POLL 5000      // Max loops to wait for Isc to stabilize
#define ISC_STABLE_ADC 5       // Stable Isc changes less than this
#define MAX_DISCARDS 300       // Maximum consecutive discarded points
#define ASPECT_HEIGHT 2        // Height of graph's aspect ratio (max 8)
#define ASPECT_WIDTH 3         // Width of graph's aspect ratio (max 8)
#define TOTAL_WEIGHT (CH1_1ST_WEIGHT + CH1_2ND_WEIGHT)
#define AVG_WEIGHT (int) ((TOTAL_WEIGHT + 1) / 2)

// Compile-time assertions
COMPILER_ASSERT(MAX_IV_MEAS <= (unsigned int) MAX_UINT);
COMPILER_ASSERT(TOTAL_WEIGHT <= 16);
COMPILER_ASSERT(ASPECT_HEIGHT <= 8);
COMPILER_ASSERT(ASPECT_WIDTH <= 8);

// Global variables
int clk_div = CLK_DIV;
int max_iv_points = MAX_IV_POINTS;
int min_isc_adc = MIN_ISC_ADC;
int max_isc_poll = MAX_ISC_POLL;
int isc_stable_adc = ISC_STABLE_ADC;
int max_discards = MAX_DISCARDS;
int aspect_height = ASPECT_HEIGHT;
int aspect_width = ASPECT_WIDTH;

void setup()
{
  boolean host_ready = FALSE;
  char incoming_msg[MAX_MSG_LEN];

  // Initialization
  pinMode(ADC_CS_PIN, OUTPUT);
  digitalWrite(ADC_CS_PIN, CS_INACTIVE);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE);
  Serial.begin(SERIAL_BAUD);
  SPI.begin();
  SPI.setClockDivider(clk_div);

  // Print version number
  Serial.print("IV Swinger2 sketch version ");
  Serial.println(VERSION);

  // Tell host that we're ready, and wait for acknowledgement
  host_ready = FALSE;
  while (!host_ready) {
    Serial.println("Ready");
    if (get_host_msg(incoming_msg)) {
      if (strstr(incoming_msg, "Ready")) {
        host_ready = TRUE;
      }
      else if (strstr(incoming_msg, "Config")) {
        process_config_msg(incoming_msg);
        Serial.println("Config processed");
      }
    }
  }
}

void loop()
{
  // Arduino: ints are 16 bits
  boolean go_msg_received;
  boolean update_prev_ch1 = FALSE;
  boolean poll_timeout = FALSE;
  char incoming_msg[MAX_MSG_LEN];
  int ii;
  int adc_ch0_delta, adc_ch1_delta;
  int manhattan_distance, min_manhattan_distance;
  unsigned int num_meas = 1; // counts IV measurements taken
  unsigned int pt_num = 1;   // counts points actually recorded
  unsigned int isc_poll_loops = 0;
  unsigned int num_discarded_pts = 0;
  unsigned int i_scale, v_scale;
  unsigned int adc_ch0_vals[MAX_IV_POINTS], adc_ch1_vals[MAX_IV_POINTS];
  unsigned int isc_adc, voc_adc, adc_offset;
  unsigned int adc_ch0_val;
  unsigned int adc_ch1_val_prev_prev, adc_ch1_val_prev, adc_ch1_val;
  unsigned long start_usecs, elapsed_usecs;
  /* unsigned long post_ch0_usecs, post_ch1_usecs, next_post_ch1_usecs; */
  float usecs_per_iv_pair;

  // Wait for go message from host
  Serial.println("Waiting for go message");
  go_msg_received = FALSE;
  while (!go_msg_received) {
    if (get_host_msg(incoming_msg)) {
      go_msg_received = TRUE;
    }
  }

  // Get Voc ADC value
  voc_adc = 0;
  for (ii = 0; ii < 20; ii++) {
    adc_ch0_val = read_adc(VOLTAGE_CH);  // Read CH0 (voltage)
    if (adc_ch0_val > voc_adc) {
      voc_adc = adc_ch0_val;
    }
  }

  // Get "zero" level for current channel (CH1). This is presumed to be
  // the same for both channels since we can't measure it for CH0.
  adc_offset = ADC_MAX;
  for (ii = 0; ii < 20; ii++) {
    adc_ch1_val = read_adc(CURRENT_CH);  // Read CH1 (current)
    if (adc_ch1_val < adc_offset) {
      adc_offset = adc_ch1_val;
    }
  }

  // Activate relay
  digitalWrite(RELAY_PIN, RELAY_ACTIVE);

  // Wait until three consecutive current measurements either decrease
  // or are equal, the difference between each pair is no more
  // than isc_stable_adc, and the current is greater than min_isc_adc.
  adc_ch1_val_prev_prev = 0;
  adc_ch1_val_prev = 0;
  poll_timeout = TRUE;
  for (ii = 0; ii < max_isc_poll; ii++) {
    adc_ch1_val = read_adc(CURRENT_CH);  // Read CH1 (current)
    // Nested ifs should be faster than &&
    if (adc_ch1_val <= adc_ch1_val_prev) {
      if (adc_ch1_val_prev <= adc_ch1_val_prev_prev) {
        if ((adc_ch1_val_prev_prev - adc_ch1_val_prev) <= isc_stable_adc) {
          if ((adc_ch1_val_prev - adc_ch1_val) <= isc_stable_adc) {
            if (adc_ch1_val > min_isc_adc) {
              poll_timeout = FALSE;
              isc_poll_loops = ii + 1;
              break;
            }
          }
        }
      }
    }
    adc_ch1_val_prev_prev = adc_ch1_val_prev;
    adc_ch1_val_prev = adc_ch1_val;
  }
  adc_ch0_val = read_adc(VOLTAGE_CH);  // Read CH0 (voltage)
  if (poll_timeout)
    Serial.println("Polling for stable Isc timed out");

  // Isc is approximately the value of the first of the three points
  // above
  isc_adc = adc_ch1_val_prev_prev;

  // First IV pair (point number 0) is last point from polling
  adc_ch0_vals[0] = adc_ch0_val;
  adc_ch1_vals[0] = adc_ch1_val;

  // Get v_scale and i_scale
  compute_v_and_i_scale(isc_adc, voc_adc, &v_scale, &i_scale);

  // Calculate the minimum scaled adc delta value. This is the Manhattan
  // distance between the Isc point and the Voc point divided by the
  // maximum number of points (minus 2 for safety). This guarantees that
  // we won't run out of memory before the complete curve is
  // captured. However, it will usually result in a number of captured
  // points that is a fair amount lower than max_iv_points. The
  // max_iv_points value is how many points there -would- be if -all-
  // points were the minimum distance apart. But some points will be
  // farther apart than the minimum distance. One reason is simply
  // because, unless max_iv_points is set to a very small number, there
  // are portions of the curve where the limiting factor is the rate
  // that the measurements can be taken; even without discarding
  // measurements, the points are farther apart than the minimum. The
  // other reason is that it is unlikely that a measurement comes at
  // exactly the mimimum distance from the previously recorded
  // measurement, so the first one that does satisfy the requirement may
  // have overshot the minimum by nearly a factor of 2:1 in the worst
  // case.
  min_manhattan_distance = ((isc_adc * i_scale) +
                            (voc_adc * v_scale)) / (max_iv_points - 2);

  // Proceed to read remaining points on IV curve. Compensate for the
  // fact that time passes between I and V measurements by using a
  // weighted average for I. Discard points that are not a minimum
  // "Manhattan distance" apart (scaled sum of V and I ADC values).
  adc_ch1_val_prev = adc_ch1_vals[0];
  start_usecs = micros();
  while (num_meas < MAX_IV_MEAS) {
    num_meas++;
    //----------------------------------------------------
    // Read both channels back-to-back. Channel 1 is first since it was
    // first in the reads for point 0 above.
    adc_ch1_val = read_adc(CURRENT_CH);  // Read CH1 (current)
    /* if (num_meas == 2) */
    /*   post_ch1_usecs = micros(); */
    /* if (num_meas == 3) */
    /*   next_post_ch1_usecs = micros(); */
    adc_ch0_val = read_adc(VOLTAGE_CH);  // Read CH0 (voltage)
    /* if (num_meas == 2) */
    /*   post_ch0_usecs = micros(); */
    //--------------------- CH1: current -----------------
    if (update_prev_ch1) {
      // Adjust previous CH1 value to weighted average with this value.
      // 16-bit integer math!! Max ADC value is 4095, so no overflow as
      // long as sum of CH1_1ST_WEIGHT and CH1_2ND_WEIGHT is 16 or less.
      adc_ch1_vals[pt_num-1] = (adc_ch1_val_prev * CH1_1ST_WEIGHT +
                                adc_ch1_val * CH1_2ND_WEIGHT +
                                AVG_WEIGHT) / TOTAL_WEIGHT;
    }
    adc_ch1_val_prev = adc_ch1_val;
    //--------------------- CH0: voltage -----------------
    adc_ch0_vals[pt_num] = adc_ch0_val;
    //------------------- Discard decision ---------------
    // "Manhattan distance" is sum of scaled deltas
    adc_ch0_delta = adc_ch0_val - adc_ch0_vals[pt_num-1];
    adc_ch1_delta = adc_ch1_vals[pt_num-1] - adc_ch1_val;
    manhattan_distance = (adc_ch0_delta * v_scale) + (adc_ch1_delta * i_scale);
    // Keep measurement if Manhattan distance is big enough; otherwise
    // discard.  However, if we've discarded max_discards consecutive
    // measurements, then keep it anyway.
    if ((manhattan_distance >= min_manhattan_distance) ||
        (num_discarded_pts >= max_discards)) {
      // Keep this one
      pt_num++;
      update_prev_ch1 = TRUE; // Adjust this CH1 value on next measurement
      num_discarded_pts = 0;  // Reset discard counter
      if (pt_num >= max_iv_points) {
        // We're done
        break;
      }
      // Nested ifs should be faster than &&
      if (adc_ch1_val < 7) {
        if (adc_ch0_delta <= 0) {
          if (adc_ch1_delta <= 0) {
            // Current value is "zero" and neither value is changing, so
            // we're done. Since we don't really know the adc_offset for
            // CH1, we use a value of <7 for "zero".
            break;
          }
        }
      }
    } else {
      // Don't record this one
      update_prev_ch1 = FALSE; // And don't adjust prev CH1 val next time
      num_discarded_pts++;
    }
  }
  if (update_prev_ch1) {
    // Last one didn't get adjusted, so do it now
    adc_ch1_vals[pt_num-1] = adc_ch1_val;
  }
  elapsed_usecs = micros() - start_usecs;
  digitalWrite(RELAY_PIN, RELAY_INACTIVE);

  // Report results on serial port
  //
  // Isc point
  Serial.print("Isc CH0:");
  Serial.print(adc_offset);
  Serial.print(" CH1:");
  Serial.println(isc_adc);
  // Middle points
  for (ii = 0; ii < pt_num; ii++) {
    Serial.print(ii);
    Serial.print(" CH0:");
    Serial.print(adc_ch0_vals[ii]);
    Serial.print(" CH1:");
    Serial.println(adc_ch1_vals[ii]);
  }
  // Voc point
  Serial.print("Voc CH0:");
  Serial.print(voc_adc);
  Serial.print(" CH1:");
  Serial.println(adc_offset);
  // Diagnostic info
  /* Serial.print("post_ch1_usecs: "); */
  /* Serial.println(post_ch1_usecs); */
  /* Serial.print("post_ch0_usecs: "); */
  /* Serial.println(post_ch0_usecs); */
  /* Serial.print("next_post_ch1_usecs: "); */
  /* Serial.println(next_post_ch1_usecs); */
  Serial.print("Isc poll loops: ");
  Serial.println(isc_poll_loops);
  Serial.print("Number of measurements: ");
  Serial.println(num_meas);
  Serial.print("Number of recorded points: ");
  Serial.println(pt_num);
  Serial.print("i_scale: ");
  Serial.println(i_scale);
  Serial.print("v_scale: ");
  Serial.println(v_scale);
  Serial.print("Elapsed usecs: ");
  Serial.println(elapsed_usecs);
  usecs_per_iv_pair = (float) elapsed_usecs / num_meas;
  Serial.print("Time (usecs) per i/v reading: ");
  Serial.println(usecs_per_iv_pair);
  Serial.println("Output complete");

}

boolean get_host_msg(char * msg) {
  boolean msg_received = FALSE;
  char c;
  int char_num = 0;
  int msg_timer;
  msg_timer = MSG_TIMER_TIMEOUT;
  while (msg_timer && !msg_received) {
    if (Serial.available()) {
      c = Serial.read();
      if (c == '\n') {
        // Substitute NULL for newline
        msg[char_num++] = '\0';
        msg_received = TRUE;
        Serial.print("Received host message: ");
        Serial.println(msg);
        break;
      } else {
        msg[char_num++] = c;
      }
      msg_timer = MSG_TIMER_TIMEOUT;
    } else {
      msg_timer--;
    }
    delay(1);
  }
  return (msg_received);
}

void process_config_msg(char * msg) {
  char *substr;
  char *config_type;
  char *config_val;
  int ii = 0;
  substr = strtok(msg, " ");  // "Config:"
  while (substr != NULL) {
    substr = strtok(NULL, " ");
    if (ii == 0) {
      config_type = substr;
    } else if (ii == 1) {
      config_val = substr;
    } else if (substr != NULL) {
      Serial.println("ERROR: Too many fields in config message");
      break;
    }
    ii++;
  }
  if (strcmp(config_type, "CLK_DIV") == 0) {
    clk_div = atoi(config_val);
    SPI.setClockDivider(clk_div);
  } else if (strcmp(config_type, "MAX_IV_POINTS") == 0) {
    max_iv_points = atoi(config_val);
  } else if (strcmp(config_type, "MIN_ISC_ADC") == 0) {
    min_isc_adc = atoi(config_val);
  } else if (strcmp(config_type, "MAX_ISC_POLL") == 0) {
    max_isc_poll = atoi(config_val);
  } else if (strcmp(config_type, "ISC_STABLE_ADC") == 0) {
    isc_stable_adc = atoi(config_val);
  } else if (strcmp(config_type, "MAX_DISCARDS") == 0) {
    max_discards = atoi(config_val);
  } else if (strcmp(config_type, "ASPECT_HEIGHT") == 0) {
    aspect_height = atoi(config_val);
  } else if (strcmp(config_type, "ASPECT_WIDTH") == 0) {
    aspect_width = atoi(config_val);
  } else {
    Serial.print("ERROR: Unknown config type: ");
    Serial.println(config_type);
  }
}

int read_adc(int ch) {
  // This code assumes MCP3202.  MCP3302 would be slightly different.
  int ms_byte, ls_byte, cmd_bytes;
  cmd_bytes = (ch == 0) ?
    B10100000 :                          // SGL/~DIFF=1, CH=0, MSBF=1
    B11100000;                           // SGL/~DIFF=1, CH=1, MSBF=1
  digitalWrite(ADC_CS_PIN, CS_ACTIVE);   // Assert active-low chip select
  SPI.transfer (B00000001);              // START=1
  ms_byte = SPI.transfer(cmd_bytes);     // Send command, get result
  ms_byte &= B00001111;                  // Bits 11:8 (mask others)
  ls_byte = SPI.transfer(0x00);          // Bits 7:0
  digitalWrite(ADC_CS_PIN, CS_INACTIVE); // Deassert active-low chip select
  return ((ms_byte << 8) | ls_byte);     // {ms_byte, lsb}
}

void compute_v_and_i_scale(unsigned int isc_adc, unsigned int voc_adc,
                           unsigned int * v_scale, unsigned int * i_scale) {

  // Find integer scaling values for V and I, with the sum of the values
  // being 16 or less.  These values are used for calculating the
  // "Manhattan distance" between points when making the discard
  // decision. The idea is that the criterion for the minimum distance
  // between points on a horizontal part of the curve should be equal to
  // the criterion for the minimum distance between points on a vertical
  // part of the curve. The distance is literally the spacing on the
  // graph. The distance between points on a diagonal part of the curve
  // is overcounted somewhat, but that results in slightly closer points
  // near the knee(s) of the curve, and that is good. The two factors
  // that determine the distance are:
  //
  //       - The maximum ADC value of the axis
  //       - The aspect ratio of the graph
  //
  // The maximum value on the X-axis (voltage) is the Voc ADC value.
  // The maximum value on the Y-axis (current) is the Isc ADC value.
  // Since the graphs are rendered in a rectangular aspect ratio, the
  // scale of the axes differs. The initial scaling values could be:
  //
  //     initial_v_scale = aspect_width / voc_adc;
  //     initial_i_scale = aspect_height / isc_adc;
  //
  // That would require large values for aspect_width and aspect_height
  // to use integer math. Instead, proportional (but much larger) values
  // can be computed with:
  //
  //     initial_v_scale = aspect_width * isc_adc;
  //     initial_i_scale = aspect_height * voc_adc;
  //
  // An algorithm is then performed to reduce the values proportionally
  // such that the sum of the values is 16 or less.
  //
  // This function is only run once, but speed is important, so 16-bit
  // integer math is used exclusively (no floats or longs).
  //

  boolean i_scale_gt_v_scale;
  unsigned int initial_v_scale, initial_i_scale;
  unsigned int lg, sm, round_up_mask;
  unsigned int lg_scale, sm_scale;
  char bit_num, shift_amt = 0;
  initial_v_scale = aspect_width * isc_adc;
  initial_i_scale = aspect_height * voc_adc;
  i_scale_gt_v_scale = initial_i_scale > initial_v_scale;
  lg = i_scale_gt_v_scale ? initial_i_scale : initial_v_scale;
  sm = i_scale_gt_v_scale ? initial_v_scale : initial_i_scale;

  // Find leftmost bit that is set in the larger initial value. The
  // right shift amount is three less than this bit number (to result in
  // a 4-bit value, i.e. 15 or less). Also look at the highest bit that
  // will be shifted off, to see if we should round up by adding one to
  // the resulting shifted amount.  If we get all the way down to bit 4
  // and it isn't set, the initial values will be used as-is.
  for (bit_num = 15; bit_num >= 4; bit_num--) {
    if (lg & (1 << bit_num)) {
      shift_amt = bit_num - 3;
      round_up_mask = (1 << (bit_num - 4));
      break;
    }
  }
  // Shift, and increment shifted amount if rounding up is needed
  lg_scale = (lg & round_up_mask) ? (lg >> shift_amt) + 1 : (lg >> shift_amt);
  sm_scale = (sm & round_up_mask) ? (sm >> shift_amt) + 1 : (sm >> shift_amt);
  // If the sum of these values is greater than 16, divide them both by
  // two (no rounding up here)
  if (lg_scale + sm_scale > 16) {
    lg_scale >>= 1;
    sm_scale >>= 1;
  }
  // Make sure sm_scale is at least 1 (necessary?)
  if (sm_scale == 0) {
    sm_scale = 1;
    if (lg_scale == 16)
      lg_scale = 15;
  }
  // Return values at pointer locations
  *v_scale = i_scale_gt_v_scale ? sm_scale : lg_scale;
  *i_scale = i_scale_gt_v_scale ? lg_scale : sm_scale;
}
