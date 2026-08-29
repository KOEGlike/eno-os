#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include "knob.h"

LOG_MODULE_REGISTER(KNOB, LOG_LEVEL_INF);

#define KNOB_NODE DT_NODELABEL(as5600)
#define VIB_MOTOR_NODE DT_ALIAS(vib_motor)

/* Rotation granularity: one selection step every 30 degrees of
 * knob travel, clockwise moves down the song list.
 */
#define DEGREES_PER_STEP 30
/* AS5600 reports 0..359 degrees, wrap detection hinges on this */
#define FULL_CIRCLE 360

/* The main loop stalls during e-ink refreshes. A delta spanning
 * such a stall is unreliable (fast rotation gets aliased by the
 * wrap logic), so any sample taken too long after the previous
 * good one re-baselines tracking instead of producing bogus steps.
 * The threshold is a few multiples of the nominal 10 ms cadence:
 * a flick at 2 rev/s covers 180 degrees in 75 ms.
 */
#define MAX_SAMPLE_GAP_MS 30

/* Sampler throttle: 30-degree navigation does not need the main
 * loop's 5 ms cadence, and every fetch spends two transactions on
 * the i2c1 bus shared with the codec and PMIC.
 */
#define KNOB_POLL_INTERVAL_MS 10

/* Clamping bounds the accumulator against quantization-noise random
 * walks (the driver truncates angles to whole degrees) while still
 * tolerating three steps of buffered travel.
 */
#define ACCUM_LIMIT (3 * DEGREES_PER_STEP)

#define HAPTIC_BRIGHTNESS_PCT 40
#define HAPTIC_PULSE_MS 40

BUILD_ASSERT(HAPTIC_BRIGHTNESS_PCT > 0 && HAPTIC_BRIGHTNESS_PCT <= 100,
	"HAPTIC_BRIGHTNESS_PCT must be a valid led brightness percent");

/* Whether clockwise rotation decreases the raw AS5600 angle depends
 * on the magnet mounting; flip this if navigation is inverted on
 * the bench.
 */
#define KNOB_INVERT 0

/* Give up turning the motor off after this many failed attempts;
 * retrying forever would silently spin the system workqueue.
 */
#define HAPTIC_OFF_MAX_RETRIES 3

static const struct device *knob_sensor = DEVICE_DT_GET(KNOB_NODE);
static const struct led_dt_spec vib_motor = LED_DT_SPEC_GET(VIB_MOTOR_NODE);

static bool have_angle;
static int last_angle;
static int angle_accum;
static int64_t last_poll_ms;
static int64_t last_good_sample_ms;

static struct k_work_delayable haptic_off_work;
static int haptic_off_retries;

static void haptic_off(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!led_off_dt(&vib_motor))
	{
		haptic_off_retries = 0;
		return;
	}

	if (++haptic_off_retries > HAPTIC_OFF_MAX_RETRIES)
	{
		LOG_ERR("Failed to turn vibration motor off");
		haptic_off_retries = 0;
		return;
	}

	/* Retry shortly instead of leaving the motor energized */
	k_work_reschedule(&haptic_off_work, K_MSEC(5));
}

int init_knob(void)
{
	if (!device_is_ready(knob_sensor))
	{
		LOG_ERR("Hall sensor %s not ready", knob_sensor->name);
		return -ENODEV;
	}

	if (!led_is_ready_dt(&vib_motor))
	{
		LOG_ERR("Vibration motor not ready");
		return -ENODEV;
	}

	/* Best effort: guarantee a known-off start */
	(void)led_off_dt(&vib_motor);

	k_work_init_delayable(&haptic_off_work, haptic_off);

	have_angle = false;
	angle_accum = 0;
	last_poll_ms = 0;
	last_good_sample_ms = 0;

	return 0;
}

void knob_haptic_pulse(void)
{
	int ret = led_set_brightness_dt(&vib_motor, HAPTIC_BRIGHTNESS_PCT);

	if (ret)
	{
		LOG_ERR("Haptic pulse failed: %d", ret);
		return;
	}

	/* The off-edge runs on the workqueue so the pulse length is
	 * independent of main-loop stalls (e-ink refreshes)
	 */
	k_work_reschedule(&haptic_off_work, K_MSEC(HAPTIC_PULSE_MS));
}

int knob_poll(void)
{
	int steps;
	int delta;
	int angle;
	struct sensor_value val;
	int64_t now = k_uptime_get();

	if (now - last_poll_ms < KNOB_POLL_INTERVAL_MS)
	{
		return 0;
	}
	last_poll_ms = now;

	if (sensor_sample_fetch(knob_sensor) || sensor_channel_get(knob_sensor, SENSOR_CHAN_ROTATION, &val))
	{
		/* Transient I2C or magnet-out-of-range failure: skip
		 * this sample. last_good_sample_ms is not advanced, so
		 * a failure window longer than MAX_SAMPLE_GAP_MS makes
		 * the next good sample re-baseline instead of producing
		 * a phantom step from the stale angle.
		 */
		return 0;
	}

	if (now - last_good_sample_ms > MAX_SAMPLE_GAP_MS)
	{
		/* Polling was stalled or interrupted: drop the stale
		 * tracking and the motion it cannot represent
		 */
		have_angle = false;
		angle_accum = 0;
	}

	/* The driver already reports 0..359, the modulo is defensive */
	angle = val.val1 % FULL_CIRCLE;

	if (!have_angle)
	{
		have_angle = true;
		last_angle = angle;
		last_good_sample_ms = now;
		return 0;
	}

	delta = angle - last_angle;
	if (delta > FULL_CIRCLE / 2)
	{
		delta -= FULL_CIRCLE;
	}
	else if (delta < -FULL_CIRCLE / 2)
	{
		delta += FULL_CIRCLE;
	}
	last_angle = angle;
	last_good_sample_ms = now;

	if (KNOB_INVERT)
	{
		delta = -delta;
	}

	angle_accum += delta;
	if (angle_accum > ACCUM_LIMIT)
	{
		angle_accum = ACCUM_LIMIT;
	}
	else if (angle_accum < -ACCUM_LIMIT)
	{
		angle_accum = -ACCUM_LIMIT;
	}

	steps = angle_accum / DEGREES_PER_STEP;
	if (steps == 0)
	{
		return 0;
	}

	angle_accum -= steps * DEGREES_PER_STEP;

	return steps;
}
