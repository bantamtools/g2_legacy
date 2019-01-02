/*
 * spindle.cpp - canonical machine spindle driver
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2014 Alden S. Hart, Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "tinyg2.h"		// #1
#include "config.h"		// #2
#include "spindle.h"
//#include "gpio.h"
#include "planner.h"
#include "hardware.h"
#include "pwm.h"
#include "report.h"
#include "util.h"
#include "math.h"
#include "settings.h"

static void _exec_spindle_control(float *value, float *flag);
static void _exec_spindle_speed(float *value, float *flag);

/*
 * cm_spindle_init()
 */
void cm_spindle_init()
{
	if( pwm.c[PWM_1].frequency < 0 )
		pwm.c[PWM_1].frequency = 0;

    pwm_set_freq(PWM_1, pwm.c[PWM_1].frequency);
    pwm_set_duty(PWM_1, pwm.c[PWM_1].phase_off);

		// Set initial RPM increment and delay - TODO config.cpp should handle this, not initializing
		cm.gm.rpm_increment 		= SP_RPM_INCREMENT;
		cm.gm.dly_per_rpm_incr 	= SP_DLY_PER_RPM_INCR;
}

/*
 * cm_get_spindle_pwm() - return PWM phase (duty cycle) for dir and speed
 */
float cm_get_spindle_pwm( uint8_t spindle_mode, float speed )
{
	float speed_lo=0, speed_hi=0, phase_lo=0, phase_hi=0;

	get_speed_limits(spindle_mode, &speed_lo, &speed_hi);
	if (spindle_mode == SPINDLE_CW ) {
		phase_lo = pwm.c[PWM_1].cw_phase_lo;
		phase_hi = pwm.c[PWM_1].cw_phase_hi;
	} else if (spindle_mode == SPINDLE_CCW ) {
		phase_lo = pwm.c[PWM_1].ccw_phase_lo;
		phase_hi = pwm.c[PWM_1].ccw_phase_hi;
	}

	if (spindle_mode==SPINDLE_CW || spindle_mode==SPINDLE_CCW ) {

		// map speed to duty cycle
#ifdef P1_USE_MAPPING_CUBIC
        // regressed cubic polynomial mapping
        float x = speed;
        float duty = P1_MAPPING_CUBIC_X0 + x * (P1_MAPPING_CUBIC_X1 + x * (P1_MAPPING_CUBIC_X2 + x * P1_MAPPING_CUBIC_X3));
#else
        // simple linear map
		float lerpfactor = (speed - speed_lo) / (speed_hi - speed_lo);
		float duty = (lerpfactor * (phase_hi - phase_lo)) + phase_lo;
#endif

				// Always clamp duty cycle to hi range
				if (duty > phase_hi) duty = phase_hi;

				// Decreasing duty cycle, clamp to lo range
				if ((cm.gm.spindle_speed < cm.gm.prev_spindle_speed) && (duty < phase_lo)) duty = phase_lo;

        return duty;
	} else {
		return pwm.c[PWM_1].phase_off;
	}
}

/*
 * get_speed_limits() - returns the speed range limits based on
 * spindle direction
 */
 stat_t get_speed_limits(uint8_t mode, float *speed_lo, float *speed_hi) {
	 if (mode == SPINDLE_CW ) {
		 *speed_lo = pwm.c[PWM_1].cw_speed_lo;
		 *speed_hi = pwm.c[PWM_1].cw_speed_hi;
	 } else if (mode == SPINDLE_CCW ) {
		 *speed_lo = pwm.c[PWM_1].ccw_speed_lo;
		 *speed_hi = pwm.c[PWM_1].ccw_speed_hi;
	 }
	 return STAT_OK;
 }

/*
 * cm_spindle_control() -  queue the spindle command to the planner buffer
 * cm_exec_spindle_control() - execute the spindle command (called from planner)
 */

stat_t cm_spindle_control(uint8_t spindle_mode)
{
	if(cm.gm.spindle_mode & SPINDLE_PAUSED)
		spindle_mode |= SPINDLE_PAUSED;
	else
		cm_cycle_start();

	float value[AXES] = { (float)spindle_mode, 0,0,0,0,0 };
	mp_queue_command(_exec_spindle_control, value, value);
	return(STAT_OK);
}

stat_t cm_spindle_control_immediate(uint8_t spindle_mode)
{
	float value[AXES] = { (float)spindle_mode, 0,0,0,0,0 };
	_exec_spindle_control(value, value);
	return (STAT_OK);
}

//static void _exec_spindle_control(uint8_t spindle_mode, float f, float *vector, float *flag)
static void _exec_spindle_control(float *value, float *flag)
{
	uint8_t spindle_mode = (uint8_t)value[0];
	float speed_lo, speed_hi;

	bool paused = spindle_mode & SPINDLE_PAUSED;
	uint8_t raw_spindle_mode = spindle_mode & (~SPINDLE_PAUSED);

	if(cm.estop_state != 0) // In E-stop, don't process any spindle commands
		spindle_mode = raw_spindle_mode = SPINDLE_OFF;
	// If we're paused or in interlock, or the esc is rebooting, send the spindle an "OFF" command (invisible to cm.gm),
	// and issue a hold if necessary
	else if((paused || cm.safety_state != 0) && raw_spindle_mode != SPINDLE_OFF) {
		if(!paused) {
			spindle_mode |= SPINDLE_PAUSED;
			cm_set_motion_state(MOTION_HOLD);
			cm.hold_state = FEEDHOLD_HOLD;
			sr_request_status_report(SR_REQUEST_IMMEDIATE);
		}
		raw_spindle_mode = SPINDLE_OFF;
	}

	cm_set_spindle_mode(MODEL, spindle_mode);

#ifdef __AVR
	if (raw_spindle_mode == SPINDLE_CW) {
		gpio_set_bit_on(SPINDLE_BIT);
		gpio_set_bit_off(SPINDLE_DIR);
	} else if (raw_spindle_mode == SPINDLE_CCW) {
		gpio_set_bit_on(SPINDLE_BIT);
		gpio_set_bit_on(SPINDLE_DIR);
	} else {
		gpio_set_bit_off(SPINDLE_BIT);	// failsafe: any error causes stop
	}
#endif // __AVR
#ifdef __ARM
	if (raw_spindle_mode == SPINDLE_CW) {
		spindle_enable_pin.set();
		spindle_dir_pin.clear();
	} else if (raw_spindle_mode == SPINDLE_CCW) {
		spindle_enable_pin.set();
		spindle_dir_pin.set();
	} else {
		spindle_enable_pin.clear();	// failsafe: any error causes stop
	}
#endif // __ARM

	// Check speed limits if spindle running
	if ((spindle_mode == SPINDLE_CW) || (spindle_mode == SPINDLE_CCW)) {

		// Get speed limits
		get_speed_limits(spindle_mode, &speed_lo, &speed_hi);

		// Clamp spindle speed to lo/hi range
		if( cm.gm.spindle_speed < speed_lo ) cm.gm.spindle_speed = speed_lo;
		if( cm.gm.spindle_speed > speed_hi ) cm.gm.spindle_speed = speed_hi;
	}

	// Perform soft-start
	cm_spindle_soft_start(spindle_mode);
}

/*
 * cm_set_spindle_speed() 	- queue the S parameter to the planner buffer
 * cm_exec_spindle_speed() 	- execute the S command (called from the planner buffer)
 * _exec_spindle_speed()	- spindle speed callback from planner queue
 */
stat_t cm_set_spindle_speed(float speed)
{
//	if (speed > cfg.max_spindle speed) { return (STAT_MAX_SPINDLE_SPEED_EXCEEDED);}
	float value[AXES] = { speed, 0,0,0,0,0 };
	mp_queue_command(_exec_spindle_speed, value, value);
	return (STAT_OK);
}

static void _exec_spindle_speed(float *value, float *flag)
{
	uint8_t spindle_mode = cm.gm.spindle_mode & (~SPINDLE_PAUSED);
	bool paused = cm.gm.spindle_mode & SPINDLE_PAUSED;
	float speed_lo, speed_hi;

	if(cm.estop_state != 0 || cm.safety_state != 0 || paused)
		spindle_mode = SPINDLE_OFF;

	cm_set_spindle_speed_parameter(MODEL, value[0]);

	// Check speed limits if spindle running
	if ((spindle_mode == SPINDLE_CW) || (spindle_mode == SPINDLE_CCW)) {

		// Get speed limits
		get_speed_limits(spindle_mode, &speed_lo, &speed_hi);

		// Clamp spindle speed to lo/hi range
		if( cm.gm.spindle_speed < speed_lo ) cm.gm.spindle_speed = speed_lo;
		if( cm.gm.spindle_speed > speed_hi ) cm.gm.spindle_speed = speed_hi;
	}

	// Perform soft-start
	cm_spindle_soft_start(spindle_mode);
}

// cm_spindle_soft_start: performs soft-start procedure for spindle
stat_t cm_spindle_soft_start(uint8_t spindle_mode) {

	float prev_speed, pwm_rpm_delta, f;
	bool paused = spindle_mode & SPINDLE_PAUSED;

	// Spindle OFF (M5) or paused, run straight pwm command without ramp to turn off
	if (spindle_mode == SPINDLE_OFF || paused) {

		// Ensure timer is off and reset
		pwm_soft_start_end();

		// Run single PWM command to turn off
		pwm_set_duty(PWM_1, cm_get_spindle_pwm(spindle_mode, cm.gm.spindle_speed) ); // update spindle speed if we're running

		// Set previous speed to zero for next time
		prev_speed = 0.0;

	// Ramp up/down to final RPM value with soft-start delay in-between.
	// Set a step only if haven't started or have completed previous ramp step.
	} else if ((spindle_mode == SPINDLE_CW || spindle_mode == SPINDLE_CCW) &&
						(cm.gm.spindle_speed != cm.gm.prev_spindle_speed) &&
						(!pwm_is_soft_start_enabled() || pwm_is_soft_start_done(cm.gm.dly_per_rpm_incr))) {

			// Check valid RPM setting
			if (cm.gm.rpm_increment <= 0) {
				return STAT_COMMAND_NOT_ACCEPTED;
			}

			// Ensure timer is off and reset
			pwm_soft_start_end();

			// Set increment/decrement based on whether increasing or decreasing speed
			if (cm.gm.spindle_speed > cm.gm.prev_spindle_speed) {	// Increase speed
				pwm_rpm_delta = cm.gm.rpm_increment;
			} else {	// Decrease speed
				pwm_rpm_delta = (-1.0 * cm.gm.rpm_increment);
			}

			// Set next step in spindle speed either to remainder or next RPM delta
			if (fabs(cm.gm.spindle_speed - cm.gm.prev_spindle_speed) < fabs(pwm_rpm_delta)) {
				f = cm.gm.spindle_speed;
			} else {
				f = cm.gm.prev_spindle_speed + pwm_rpm_delta;
			}

			// Update spindle speed if we're running; delay on valid duty cycle (cubic)
			if (pwm_set_duty(PWM_1, cm_get_spindle_pwm(spindle_mode, f)) == STAT_OK) {
				pwm_soft_start_begin();
				//delay_test_pin.toggle();
			}

			// Set previous speed to current increment
			prev_speed = f;

	// In delay, previous speed should remain the same
	} else {
		prev_speed = cm.gm.prev_spindle_speed;
	}

	// Set previous speed
	cm_set_prev_spindle_speed_parameter(MODEL, prev_speed);

	return STAT_OK;
}
