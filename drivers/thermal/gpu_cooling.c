/*
 *  linux/drivers/thermal/gpu_cooling.c
 *
 *  Copyright (C) 2012	Samsung Electronics Co., Ltd(http://www.samsung.com)
 *  Copyright (C) 2012  Amit Daniel <amit.kachhap@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/gpu_cooling.h>
#include <soc/samsung/tmu.h>
#include <trace/events/thermal.h>

#include <soc/samsung/cal-if.h>
#include <soc/samsung/ect_parser.h>
#include "samsung/exynos_tmu.h"

#if defined(CONFIG_SOC_EXYNOS8895) && defined(CONFIG_SOC_EMULATOR8895)
#include <dt-bindings/clock/emulator8895.h>
#elif defined(CONFIG_SOC_EXYNOS8895) && !defined(CONFIG_SOC_EMULATOR8895)
#include <dt-bindings/clock/exynos8895.h>
#elif defined(CONFIG_SOC_EXYNOS7872)
#include <dt-bindings/clock/exynos7872.h>
#endif

/**
 * struct power_table - frequency to power conversion
 * @frequency:	frequency in KHz
 * @power:	power in mW
 *
 * This structure is built when the cooling device registers and helps
 * in translating frequency to power and viceversa.
 */
struct power_table {
	u32 frequency;
	u32 power;
};

/**
 * struct gpufreq_cooling_device - data for cooling device with gpufreq
 * @id: unique integer value corresponding to each gpufreq_cooling_device
 *	registered.
 * @cool_dev: thermal_cooling_device pointer to keep track of the
 *	registered cooling device.
 * @gpufreq_state: integer value representing the current state of gpufreq
 *	cooling	devices.
 * @gpufreq_val: integer value representing the absolute value of the clipped
 *	frequency.
 * @allowed_gpus: all the gpus involved for this gpufreq_cooling_device.
 *
 * This structure is required for keeping information of each
 * gpufreq_cooling_device registered. In order to prevent corruption of this a
 * mutex lock cooling_gpu_lock is used.
 */
struct gpufreq_cooling_device {
	int id;
	struct thermal_cooling_device *cool_dev;
	unsigned long gpufreq_state;
	unsigned int gpufreq_val;
	u32 last_load;
	struct power_table *dyn_power_table;
	int dyn_power_table_entries;
	get_static_t plat_get_static_power;
	int *var_table;
	int *var_coeff;
	int *asv_coeff;
	unsigned int var_volt_size;
	unsigned int var_temp_size;
};

static DEFINE_IDR(gpufreq_idr);
static DEFINE_MUTEX(cooling_gpu_lock);
static BLOCKING_NOTIFIER_HEAD(gpu_notifier);

static unsigned int gpufreq_dev_count;

struct cpufreq_frequency_table *gpu_freq_table;

/**
 * get_idr - function to get a unique id.
 * @idr: struct idr * handle used to create a id.
 * @id: int * value generated by this function.
 *
 * This function will populate @id with an unique
 * id, using the idr API.
 *
 * Return: 0 on success, an error code on failure.
 */
static int get_idr(struct idr *idr, int *id)
{
	int ret;

	mutex_lock(&cooling_gpu_lock);
	ret = idr_alloc(idr, NULL, 0, 0, GFP_KERNEL);
	mutex_unlock(&cooling_gpu_lock);
	if (unlikely(ret < 0))
		return ret;
	*id = ret;

	return 0;
}

/**
 * release_idr - function to free the unique id.
 * @idr: struct idr * handle used for creating the id.
 * @id: int value representing the unique id.
 */
static void release_idr(struct idr *idr, int id)
{
	mutex_lock(&cooling_gpu_lock);
	idr_remove(idr, id);
	mutex_unlock(&cooling_gpu_lock);
}

/* Below code defines functions to be used for gpufreq as cooling device */

enum gpufreq_cooling_property {
	GET_LEVEL,
	GET_FREQ,
	GET_MAXL,
};

/**
 * get_property - fetch a property of interest for a give gpu.
 * @gpu: gpu for which the property is required
 * @input: query parameter
 * @output: query return
 * @property: type of query (frequency, level, max level)
 *
 * This is the common function to
 * 1. get maximum gpu cooling states
 * 2. translate frequency to cooling state
 * 3. translate cooling state to frequency
 * Note that the code may be not in good shape
 * but it is written in this way in order to:
 * a) reduce duplicate code as most of the code can be shared.
 * b) make sure the logic is consistent when translating between
 *    cooling states and frequencies.
 *
 * Return: 0 on success, -EINVAL when invalid parameters are passed.
 */
static int get_property(unsigned int gpu, unsigned long input,
			unsigned int *output,
			enum gpufreq_cooling_property property)
{
	int i;
	unsigned long max_level = 0, level = 0;
	unsigned int freq = CPUFREQ_ENTRY_INVALID;
	int descend = -1;
	struct cpufreq_frequency_table *pos, *table =
					gpu_freq_table;

	if (!output)
		return -EINVAL;

	if (!table)
		return -EINVAL;

	cpufreq_for_each_valid_entry(pos, table) {
		/* ignore duplicate entry */
		if (freq == pos->frequency)
			continue;

		/* get the frequency order */
		if (freq != CPUFREQ_ENTRY_INVALID && descend == -1)
			descend = freq > pos->frequency;

		freq = pos->frequency;
		max_level++;
	}

	/* No valid cpu frequency entry */
	if (max_level == 0)
		return -EINVAL;

	/* max_level is an index, not a counter */
	max_level--;

	/* get max level */
	if (property == GET_MAXL) {
		*output = (unsigned int)max_level;
		return 0;
	}

	if (property == GET_FREQ)
		level = descend ? input : (max_level - input);

	i = 0;
	cpufreq_for_each_valid_entry(pos, table) {
		/* ignore duplicate entry */
		if (freq == pos->frequency)
			continue;

		/* now we have a valid frequency entry */
		freq = pos->frequency;

		if (property == GET_LEVEL && (unsigned int)input == freq) {
			/* get level by frequency */
			*output = descend ? i : (max_level - i);
			return 0;
		}
		if (property == GET_FREQ && level == i) {
			/* get frequency by level */
			*output = freq;
			return 0;
		}
		i++;
	}

	return -EINVAL;
}

/**
 * gpufreq_cooling_get_level - for a give gpu, return the cooling level.
 * @gpu: gpu for which the level is required
 * @freq: the frequency of interest
 *
 * This function will match the cooling level corresponding to the
 * requested @freq and return it.
 *
 * Return: The matched cooling level on success or THERMAL_CSTATE_INVALID
 * otherwise.
 */
unsigned long gpufreq_cooling_get_level(unsigned int gpu, unsigned int freq)
{
	unsigned int val;

	if (get_property(gpu, (unsigned long)freq, &val, GET_LEVEL))
		return THERMAL_CSTATE_INVALID;

	return (unsigned long)val;
}
EXPORT_SYMBOL_GPL(gpufreq_cooling_get_level);

/**
 * gpufreq_cooling_get_freq - for a give gpu, return the cooling frequency.
 * @gpu: gpu for which the level is required
 * @level: the level of interest
 *
 * This function will match the cooling level corresponding to the
 * requested @freq and return it.
 *
 * Return: The matched cooling level on success or THERMAL_CFREQ_INVALID
 * otherwise.
 */
static u32 gpufreq_cooling_get_freq(unsigned int gpu, unsigned long level)
{
	unsigned int val = 0;

	if (get_property(gpu, level, &val, GET_FREQ))
		return THERMAL_CFREQ_INVALID;

	return val;
}
EXPORT_SYMBOL_GPL(gpufreq_cooling_get_level);

/**
 * build_dyn_power_table() - create a dynamic power to frequency table
 * @gpufreq_device:	the gpufreq cooling device in which to store the table
 * @capacitance: dynamic power coefficient for these gpus
 *
 * Build a dynamic power to frequency table for this gpu and store it
 * in @gpufreq_device.  This table will be used in gpu_power_to_freq() and
 * gpu_freq_to_power() to convert between power and frequency
 * efficiently.  Power is stored in mW, frequency in KHz.  The
 * resulting table is in ascending order.
 *
 * Return: 0 on success, -EINVAL if there are no OPPs for any CPUs,
 * -ENOMEM if we run out of memory or -EAGAIN if an OPP was
 * added/enabled while the function was executing.
 */
static int build_dyn_power_table(struct gpufreq_cooling_device *gpufreq_device,
				 u32 capacitance)
{
	struct power_table *power_table;
	int num_opps = 0, i, cnt = 0;
	unsigned long freq;

	num_opps = gpu_dvfs_get_step();

	if (num_opps == 0)
		return -EINVAL;

	power_table = kcalloc(num_opps, sizeof(*power_table), GFP_KERNEL);
	if (!power_table)
		return -ENOMEM;

	for (freq = 0, i = 0; i < num_opps; i++) {
		u32 voltage_mv;
		u64 power;

		freq = gpu_dvfs_get_clock(num_opps - i - 1);

		if (freq > gpu_dvfs_get_max_freq())
			continue;

		voltage_mv = gpu_dvfs_get_voltage(freq) / 1000;

		/*
		 * Do the multiplication with MHz and millivolt so as
		 * to not overflow.
		 */
		power = (u64)capacitance * freq * voltage_mv * voltage_mv;
		do_div(power, 1000000000);

		power_table[i].frequency = freq;

		/* power is stored in mW */
		power_table[i].power = power;
		cnt++;
	}

	gpufreq_device->dyn_power_table = power_table;
	gpufreq_device->dyn_power_table_entries = cnt;

	return 0;
}

static int build_static_power_table(struct gpufreq_cooling_device *gpufreq_device)
{
	int i, j;
	int ratio = cal_asv_get_ids_info(ACPM_DVFS_G3D);
	int asv_group = cal_asv_get_grp(ACPM_DVFS_G3D);
	void *gen_block;
	struct ect_gen_param_table *volt_temp_param, *asv_param;
	int ratio_table[16] = { 0, 25, 29, 35, 41, 48, 57, 67, 79, 94, 110, 130, 151, 162, 162, 162};

	if (!ratio)
		ratio = ratio_table[asv_group];

	gen_block = ect_get_block("GEN");
	if (gen_block == NULL) {
		pr_err("%s: Failed to get gen block from ECT\n", __func__);
		return -EINVAL;
	}

	volt_temp_param = ect_gen_param_get_table(gen_block, "DTM_G3D_VOLT_TEMP");
	asv_param = ect_gen_param_get_table(gen_block, "DTM_G3D_ASV");

	if (volt_temp_param && asv_param) {
		gpufreq_device->var_volt_size = volt_temp_param->num_of_row - 1;
		gpufreq_device->var_temp_size = volt_temp_param->num_of_col - 1;

		gpufreq_device->var_coeff = kzalloc(sizeof(int) *
							volt_temp_param->num_of_row *
							volt_temp_param->num_of_col,
							GFP_KERNEL);
		if (!gpufreq_device->var_coeff)
			goto err_mem;

		gpufreq_device->asv_coeff = kzalloc(sizeof(int) *
							asv_param->num_of_row *
							asv_param->num_of_col,
							GFP_KERNEL);
		if (!gpufreq_device->asv_coeff)
			goto free_var_coeff;

		gpufreq_device->var_table = kzalloc(sizeof(int) *
							volt_temp_param->num_of_row *
							volt_temp_param->num_of_col,
							GFP_KERNEL);
		if (!gpufreq_device->var_table)
			goto free_asv_coeff;

		memcpy(gpufreq_device->var_coeff, volt_temp_param->parameter,
			sizeof(int) * volt_temp_param->num_of_row * volt_temp_param->num_of_col);
		memcpy(gpufreq_device->asv_coeff, asv_param->parameter,
			sizeof(int) * asv_param->num_of_row * asv_param->num_of_col);
		memcpy(gpufreq_device->var_table, volt_temp_param->parameter,
			sizeof(int) * volt_temp_param->num_of_row * volt_temp_param->num_of_col);
	} else {
		pr_err("%s: Failed to get param table from ECT\n", __func__);
		return -EINVAL;
	}

	for (i = 1; i <= gpufreq_device->var_volt_size; i++) {
		long asv_coeff = (long)gpufreq_device->asv_coeff[3 * i + 0] * asv_group * asv_group
				+ (long)gpufreq_device->asv_coeff[3 * i + 1] * asv_group
				+ (long)gpufreq_device->asv_coeff[3 * i + 2];
		asv_coeff = asv_coeff / 100;

		for (j = 1; j <= gpufreq_device->var_temp_size; j++) {
			long var_coeff = (long)gpufreq_device->var_coeff[i * (gpufreq_device->var_temp_size + 1) + j];
			var_coeff =  ratio * var_coeff * asv_coeff;
			var_coeff = var_coeff / 100000;
			gpufreq_device->var_table[i * (gpufreq_device->var_temp_size + 1) + j] = (int)var_coeff;
		}
	}

	return 0;

free_asv_coeff:
	kfree(gpufreq_device->asv_coeff);
free_var_coeff:
	kfree(gpufreq_device->var_coeff);
err_mem:
	return -ENOMEM;
}

static int lookup_static_power(struct gpufreq_cooling_device *gpufreq_device,
		unsigned long voltage, int temperature, u32 *power)
{
	int volt_index = 0, temp_index = 0;

	voltage = voltage / 1000;
	temperature  = temperature / 1000;

	for (volt_index = 0; volt_index <= gpufreq_device->var_volt_size; volt_index++) {
		if (voltage < gpufreq_device->var_table[volt_index * (gpufreq_device->var_temp_size + 1)]) {
			volt_index = volt_index - 1;
			break;
		}
	}

	if (volt_index == 0)
		volt_index = 1;

	if (volt_index > gpufreq_device->var_volt_size)
		volt_index = gpufreq_device->var_volt_size;

	for (temp_index = 0; temp_index <= gpufreq_device->var_temp_size; temp_index++) {
		if (temperature < gpufreq_device->var_table[temp_index]) {
			temp_index = temp_index - 1;
			break;
		}
	}

	if (temp_index == 0)
		temp_index = 1;

	if (temp_index > gpufreq_device->var_temp_size)
		temp_index = gpufreq_device->var_temp_size;

	*power = (unsigned int)gpufreq_device->var_table[volt_index * (gpufreq_device->var_temp_size + 1) + temp_index];

	return 0;
}

static u32 gpu_freq_to_power(struct gpufreq_cooling_device *gpufreq_device,
			     u32 freq)
{
	int i;
	struct power_table *pt = gpufreq_device->dyn_power_table;

	for (i = 1; i < gpufreq_device->dyn_power_table_entries; i++)
		if (freq < pt[i].frequency)
			break;

	return pt[i - 1].power;
}

static u32 gpu_power_to_freq(struct gpufreq_cooling_device *gpufreq_device,
			     u32 power)
{
	int i;
	struct power_table *pt = gpufreq_device->dyn_power_table;

	for (i = 1; i < gpufreq_device->dyn_power_table_entries; i++)
		if (power < pt[i].power)
			break;

	return pt[i - 1].frequency;
}

/**
 * get_static_power() - calculate the static power consumed by the gpus
 * @gpufreq_device:	struct &gpufreq_cooling_device for this gpu cdev
 * @tz:		thermal zone device in which we're operating
 * @freq:	frequency in KHz
 * @power:	pointer in which to store the calculated static power
 *
 * Calculate the static power consumed by the gpus described by
 * @gpu_actor running at frequency @freq.  This function relies on a
 * platform specific function that should have been provided when the
 * actor was registered.  If it wasn't, the static power is assumed to
 * be negligible.  The calculated static power is stored in @power.
 *
 * Return: 0 on success, -E* on failure.
 */
static int get_static_power(struct gpufreq_cooling_device *gpufreq_device,
			    struct thermal_zone_device *tz, unsigned long freq,
			    u32 *power)
{
	unsigned long voltage;

	if (freq == 0) {
		*power = 0;
		return 0;
	}

	voltage = gpu_dvfs_get_voltage(freq);

	if (voltage == 0) {
		pr_warn("Failed to get voltage for frequency %lu\n", freq);
		return -EINVAL;
	}

	return lookup_static_power(gpufreq_device, voltage, tz->temperature, power);
}

/**
 * get_dynamic_power() - calculate the dynamic power
 * @gpufreq_device:	&gpufreq_cooling_device for this cdev
 * @freq:	current frequency
 *
 * Return: the dynamic power consumed by the gpus described by
 * @gpufreq_device.
 */
static u32 get_dynamic_power(struct gpufreq_cooling_device *gpufreq_device,
			     unsigned long freq)
{
	u32 raw_gpu_power;

	raw_gpu_power = gpu_freq_to_power(gpufreq_device, freq);
	return (raw_gpu_power * gpufreq_device->last_load) / 100;
}

/**
 * gpufreq_apply_cooling - function to apply frequency clipping.
 * @gpufreq_device: gpufreq_cooling_device pointer containing frequency
 *	clipping data.
 * @cooling_state: value of the cooling state.
 *
 * Function used to make sure the gpufreq layer is aware of current thermal
 * limits. The limits are applied by updating the gpufreq policy.
 *
 * Return: 0 on success, an error code otherwise (-EINVAL in case wrong
 * cooling state).
 */
static int gpufreq_apply_cooling(struct gpufreq_cooling_device *gpufreq_device,
				 unsigned long cooling_state)
{
	unsigned int gpu_cooling_freq = 0;

	/* Check if the old cooling action is same as new cooling action */
	if (gpufreq_device->gpufreq_state == cooling_state)
		return 0;

	gpufreq_device->gpufreq_state = cooling_state;

	gpu_cooling_freq = gpufreq_cooling_get_freq(0, gpufreq_device->gpufreq_state);
	if (gpu_cooling_freq == THERMAL_CFREQ_INVALID) {
		pr_warn("Failed to convert %lu gpu_level\n",
				     gpufreq_device->gpufreq_state);
		return -EINVAL;
	}

	gpu_cooling_freq = gpu_cooling_freq / 1000;
	blocking_notifier_call_chain(&gpu_notifier, GPU_THROTTLING, &gpu_cooling_freq);

	return 0;
}

/* gpufreq cooling device callback functions are defined below */

/**
 * gpufreq_get_max_state - callback function to get the max cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the max cooling state.
 *
 * Callback for the thermal cooling device to return the gpufreq
 * max cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int gpufreq_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	unsigned int count = 0;
	int ret;

	ret = get_property(0, 0, &count, GET_MAXL);

	if (count > 0)
		*state = count;

	return ret;
}

/**
 * gpufreq_get_cur_state - callback function to get the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: fill this variable with the current cooling state.
 *
 * Callback for the thermal cooling device to return the gpufreq
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int gpufreq_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct gpufreq_cooling_device *gpufreq_device = cdev->devdata;

	*state = gpufreq_device->gpufreq_state;

	return 0;
}

/**
 * gpufreq_set_cur_state - callback function to set the current cooling state.
 * @cdev: thermal cooling device pointer.
 * @state: set this variable to the current cooling state.
 *
 * Callback for the thermal cooling device to change the gpufreq
 * current cooling state.
 *
 * Return: 0 on success, an error code otherwise.
 */
static int gpufreq_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct gpufreq_cooling_device *gpufreq_device = cdev->devdata;

	return gpufreq_apply_cooling(gpufreq_device, state);
}

static enum gpu_noti_state_t gpu_tstate = GPU_COLD;

static int gpufreq_set_cur_temp(struct thermal_cooling_device *cdev,
				bool suspended, int temp)
{
	enum gpu_noti_state_t tstate;
	unsigned long value;

	if (suspended || temp < EXYNOS_COLD_TEMP)
		tstate = GPU_COLD;
	else
		tstate = GPU_NORMAL;

	if (gpu_tstate == tstate)
		return 0;

	gpu_tstate = tstate;
	value = tstate;

	blocking_notifier_call_chain(&gpu_notifier, tstate, &value);

	return 0;
}

/**
 * gpufreq_get_requested_power() - get the current power
 * @cdev:	&thermal_cooling_device pointer
 * @tz:		a valid thermal zone device pointer
 * @power:	pointer in which to store the resulting power
 *
 * Calculate the current power consumption of the gpus in milliwatts
 * and store it in @power.  This function should actually calculate
 * the requested power, but it's hard to get the frequency that
 * gpufreq would have assigned if there were no thermal limits.
 * Instead, we calculate the current power on the assumption that the
 * immediate future will look like the immediate past.
 *
 * We use the current frequency and the average load since this
 * function was last called.  In reality, there could have been
 * multiple opps since this function was last called and that affects
 * the load calculation.  While it's not perfectly accurate, this
 * simplification is good enough and works.  REVISIT this, as more
 * complex code may be needed if experiments show that it's not
 * accurate enough.
 *
 * Return: 0 on success, -E* if getting the static power failed.
 */
static int gpufreq_get_requested_power(struct thermal_cooling_device *cdev,
				       struct thermal_zone_device *tz,
				       u32 *power)
{
	unsigned long freq;
	int ret = 0;
	u32 static_power, dynamic_power;
	struct gpufreq_cooling_device *gpufreq_device = cdev->devdata;
	u32 load_gpu = 0;

	freq = gpu_dvfs_get_cur_clock();

	load_gpu = gpu_dvfs_get_utilization();;

	gpufreq_device->last_load = load_gpu;

	dynamic_power = get_dynamic_power(gpufreq_device, freq);
	ret = get_static_power(gpufreq_device, tz, freq, &static_power);

	if (ret)
		return ret;

	if (trace_thermal_power_gpu_get_power_enabled()) {
		trace_thermal_power_gpu_get_power(
			freq, load_gpu, dynamic_power, static_power);
	}

	*power = static_power + dynamic_power;
	return 0;
}

/**
 * gpufreq_state2power() - convert a gpu cdev state to power consumed
 * @cdev:	&thermal_cooling_device pointer
 * @tz:		a valid thermal zone device pointer
 * @state:	cooling device state to be converted
 * @power:	pointer in which to store the resulting power
 *
 * Convert cooling device state @state into power consumption in
 * milliwatts assuming 100% load.  Store the calculated power in
 * @power.
 *
 * Return: 0 on success, -EINVAL if the cooling device state could not
 * be converted into a frequency or other -E* if there was an error
 * when calculating the static power.
 */
static int gpufreq_state2power(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz,
			       unsigned long state, u32 *power)
{
	unsigned int freq;
	u32 static_power, dynamic_power;
	int ret;
	struct gpufreq_cooling_device *gpufreq_device = cdev->devdata;

	freq = gpu_freq_table[state].frequency / 1000;
	if (!freq)
		return -EINVAL;

	dynamic_power = gpu_freq_to_power(gpufreq_device, freq);
	ret = get_static_power(gpufreq_device, tz, freq, &static_power);
	if (ret)
		return ret;

	*power = static_power + dynamic_power;
	return 0;
}

/**
 * gpufreq_power2state() - convert power to a cooling device state
 * @cdev:	&thermal_cooling_device pointer
 * @tz:		a valid thermal zone device pointer
 * @power:	power in milliwatts to be converted
 * @state:	pointer in which to store the resulting state
 *
 * Calculate a cooling device state for the gpus described by @cdev
 * that would allow them to consume at most @power mW and store it in
 * @state.  Note that this calculation depends on external factors
 * such as the gpu load or the current static power.  Calling this
 * function with the same power as input can yield different cooling
 * device states depending on those external factors.
 *
 * Return: 0 on success, -ENODEV if no gpus are online or -EINVAL if
 * the calculated frequency could not be converted to a valid state.
 * The latter should not happen unless the frequencies available to
 * gpufreq have changed since the initialization of the gpu cooling
 * device.
 */
static int gpufreq_power2state(struct thermal_cooling_device *cdev,
			       struct thermal_zone_device *tz, u32 power,
			       unsigned long *state)
{
	unsigned int cur_freq, target_freq;
	int ret;
	s32 dyn_power;
	u32 static_power;
	struct gpufreq_cooling_device *gpufreq_device = cdev->devdata;

	cur_freq = gpu_dvfs_get_cur_clock();
	ret = get_static_power(gpufreq_device, tz, cur_freq, &static_power);
	if (ret)
		return ret;

	dyn_power = power - static_power;
	dyn_power = dyn_power > 0 ? dyn_power : 0;
	target_freq = gpu_power_to_freq(gpufreq_device, dyn_power);

	*state = gpufreq_cooling_get_level(0, target_freq * 1000);
	if (*state == THERMAL_CSTATE_INVALID) {
		pr_warn("Failed to convert %dKHz for gpu into a cdev state\n",
				     target_freq);
		return -EINVAL;
	}

	trace_thermal_power_gpu_limit(target_freq, *state, power);
	return 0;
}

/* Bind gpufreq callbacks to thermal cooling device ops */
static struct thermal_cooling_device_ops gpufreq_cooling_ops = {
	.get_max_state = gpufreq_get_max_state,
	.get_cur_state = gpufreq_get_cur_state,
	.set_cur_state = gpufreq_set_cur_state,
	.set_cur_temp = gpufreq_set_cur_temp,
};


int exynos_gpu_add_notifier(struct notifier_block *n)
{
	return blocking_notifier_chain_register(&gpu_notifier, n);
}

/**
 * __gpufreq_cooling_register - helper function to create gpufreq cooling device
 * @np: a valid struct device_node to the cooling device device tree node
 * @clip_gpus: gpumask of gpus where the frequency constraints will happen.
 * @capacitance: dynamic power coefficient for these gpus
 * @plat_static_func: function to calculate the static power consumed by these
 *                    gpus (optional)
 *
 * This interface function registers the gpufreq cooling device with the name
 * "thermal-gpufreq-%x". This api can support multiple instances of gpufreq
 * cooling devices. It also gives the opportunity to link the cooling device
 * with a device tree node, in order to bind it via the thermal DT code.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
static struct thermal_cooling_device *
__gpufreq_cooling_register(struct device_node *np,
			   const struct cpumask *clip_gpus, u32 capacitance,
			   get_static_t plat_static_func)
{
	struct thermal_cooling_device *cool_dev;
	struct gpufreq_cooling_device *gpufreq_dev = NULL;
	char dev_name[THERMAL_NAME_LENGTH];
	int ret = 0;

	gpufreq_dev = kzalloc(sizeof(struct gpufreq_cooling_device),
			      GFP_KERNEL);
	if (!gpufreq_dev)
		return ERR_PTR(-ENOMEM);

	ret = get_idr(&gpufreq_idr, &gpufreq_dev->id);
	if (ret) {
		kfree(gpufreq_dev);
		return ERR_PTR(-EINVAL);
	}

	if (capacitance) {
		gpufreq_cooling_ops.get_requested_power =
			gpufreq_get_requested_power;
		gpufreq_cooling_ops.state2power = gpufreq_state2power;
		gpufreq_cooling_ops.power2state = gpufreq_power2state;

		ret = build_dyn_power_table(gpufreq_dev, capacitance);

		if (ret)
			return ERR_PTR(ret);

		ret = build_static_power_table(gpufreq_dev);
		if (ret)
			return ERR_PTR(ret);
	}

	snprintf(dev_name, sizeof(dev_name), "thermal-gpufreq-%d",
		 gpufreq_dev->id);

	cool_dev = thermal_of_cooling_device_register(np, dev_name, gpufreq_dev,
						      &gpufreq_cooling_ops);
	if (IS_ERR(cool_dev)) {
		release_idr(&gpufreq_idr, gpufreq_dev->id);
		kfree(gpufreq_dev);
		return cool_dev;
	}
	gpufreq_dev->cool_dev = cool_dev;
	gpufreq_dev->gpufreq_state = 0;
	mutex_lock(&cooling_gpu_lock);

	gpufreq_dev_count++;

	mutex_unlock(&cooling_gpu_lock);

	return cool_dev;
}

/**
 * gpufreq_cooling_register - function to create gpufreq cooling device.
 * @clip_gpus: cpumask of gpus where the frequency constraints will happen.
 *
 * This interface function registers the gpufreq cooling device with the name
 * "thermal-gpufreq-%x". This api can support multiple instances of gpufreq
 * cooling devices.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
gpufreq_cooling_register(const struct cpumask *clip_gpus)
{
	return __gpufreq_cooling_register(NULL, clip_gpus, 0, NULL);
}
EXPORT_SYMBOL_GPL(gpufreq_cooling_register);

/**
 * of_gpufreq_cooling_register - function to create gpufreq cooling device.
 * @np: a valid struct device_node to the cooling device device tree node
 * @clip_gpus: cpumask of gpus where the frequency constraints will happen.
 *
 * This interface function registers the gpufreq cooling device with the name
 * "thermal-gpufreq-%x". This api can support multiple instances of gpufreq
 * cooling devices. Using this API, the gpufreq cooling device will be
 * linked to the device tree node provided.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
of_gpufreq_cooling_register(struct device_node *np,
			    const struct cpumask *clip_gpus)
{
	if (!np)
		return ERR_PTR(-EINVAL);

	return __gpufreq_cooling_register(np, clip_gpus, 0, NULL);
}
EXPORT_SYMBOL_GPL(of_gpufreq_cooling_register);

/**
 * gpufreq_power_cooling_register() - create gpufreq cooling device with power extensions
 * @clip_gpus:	gpumask of gpus where the frequency constraints will happen
 * @capacitance:	dynamic power coefficient for these gpus
 * @plat_static_func:	function to calculate the static power consumed by these
 *			gpus (optional)
 *
 * This interface function registers the gpufreq cooling device with
 * the name "thermal-gpufreq-%x".  This api can support multiple
 * instances of gpufreq cooling devices.  Using this function, the
 * cooling device will implement the power extensions by using a
 * simple gpu power model.  The gpus must have registered their OPPs
 * using the OPP library.
 *
 * An optional @plat_static_func may be provided to calculate the
 * static power consumed by these gpus.  If the platform's static
 * power consumption is unknown or negligible, make it NULL.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
gpufreq_power_cooling_register(const struct cpumask *clip_gpus, u32 capacitance,
			       get_static_t plat_static_func)
{
	return __gpufreq_cooling_register(NULL, clip_gpus, capacitance,
				plat_static_func);
}
EXPORT_SYMBOL(gpufreq_power_cooling_register);

/**
 * of_gpufreq_power_cooling_register() - create gpufreq cooling device with power extensions
 * @np:	a valid struct device_node to the cooling device device tree node
 * @clip_gpus:	gpumask of gpus where the frequency constraints will happen
 * @capacitance:	dynamic power coefficient for these gpus
 * @plat_static_func:	function to calculate the static power consumed by these
 *			gpus (optional)
 *
 * This interface function registers the gpufreq cooling device with
 * the name "thermal-gpufreq-%x".  This api can support multiple
 * instances of gpufreq cooling devices.  Using this API, the gpufreq
 * cooling device will be linked to the device tree node provided.
 * Using this function, the cooling device will implement the power
 * extensions by using a simple gpu power model.  The gpus must have
 * registered their OPPs using the OPP library.
 *
 * An optional @plat_static_func may be provided to calculate the
 * static power consumed by these gpus.  If the platform's static
 * power consumption is unknown or negligible, make it NULL.
 *
 * Return: a valid struct thermal_cooling_device pointer on success,
 * on failure, it returns a corresponding ERR_PTR().
 */
struct thermal_cooling_device *
of_gpufreq_power_cooling_register(struct device_node *np,
				  const struct cpumask *clip_gpus,
				  u32 capacitance,
				  get_static_t plat_static_func)
{
	if (!np)
		return ERR_PTR(-EINVAL);

	return __gpufreq_cooling_register(np, clip_gpus, capacitance,
				plat_static_func);
}
EXPORT_SYMBOL(of_gpufreq_power_cooling_register);


/**
 * gpufreq_cooling_unregister - function to remove gpufreq cooling device.
 * @cdev: thermal cooling device pointer.
 *
 * This interface function unregisters the "thermal-gpufreq-%x" cooling device.
 */
void gpufreq_cooling_unregister(struct thermal_cooling_device *cdev)
{
	struct gpufreq_cooling_device *gpufreq_dev;

	if (!cdev)
		return;

	gpufreq_dev = cdev->devdata;
	mutex_lock(&cooling_gpu_lock);
	gpufreq_dev_count--;
	mutex_unlock(&cooling_gpu_lock);

	thermal_cooling_device_unregister(gpufreq_dev->cool_dev);
	release_idr(&gpufreq_idr, gpufreq_dev->id);
	kfree(gpufreq_dev);
}
EXPORT_SYMBOL_GPL(gpufreq_cooling_unregister);

/**
 * gpu_cooling_table_init - function to make GPU throttling table.
 * @pdev : struct platform_device pointer
 *
 * Return : a valid struct gpu_freq_table pointer on success,
 * on failture, it returns a corresponding ERR_PTR().
 */
int gpu_cooling_table_init(struct platform_device *pdev)
{
	int ret = 0, i = 0;
#if defined(CONFIG_ECT)
	struct exynos_tmu_data *exynos_data;
	void *thermal_block;
	struct ect_ap_thermal_function *function;
	int last_level = -1, count = 0;
#else
	unsigned int table_size;
	u32 gpu_idx_num = 0;
#endif

#if defined(CONFIG_ECT)
	exynos_data = platform_get_drvdata(pdev);

	thermal_block = ect_get_block(BLOCK_AP_THERMAL);
	if (thermal_block == NULL) {
		dev_err(&pdev->dev, "Failed to get thermal block");
		return -ENODEV;
	}

	function = ect_ap_thermal_get_function(thermal_block, exynos_data->tmu_name);
	if (function == NULL) {
		dev_err(&pdev->dev, "Failed to get %s information", exynos_data->tmu_name);
		return -ENODEV;
	}

	/* Table size can be num_of_range + 1 since last row has the value of TABLE_END */
	gpu_freq_table = kzalloc(sizeof(struct cpufreq_frequency_table)
					* (function->num_of_range + 1), GFP_KERNEL);

	for (i = 0; i < function->num_of_range; i++) {
		if (last_level == function->range_list[i].max_frequency)
			continue;

		gpu_freq_table[count].flags = 0;
		gpu_freq_table[count].driver_data = count;
		gpu_freq_table[count].frequency = function->range_list[i].max_frequency;
		last_level = gpu_freq_table[count].frequency;

		dev_info(&pdev->dev, "[GPU TMU] index : %d, frequency : %d \n",
			gpu_freq_table[count].driver_data, gpu_freq_table[count].frequency);
		count++;
	}

	if (i == function->num_of_range)
		gpu_freq_table[count].frequency = GPU_TABLE_END;
#else
	/* gpu cooling frequency table parse */
	ret = of_property_read_u32(pdev->dev.of_node, "gpu_idx_num", &gpu_idx_num);
	if (ret < 0)
		dev_err(&pdev->dev, "gpu_idx_num happend error value\n");

	if (gpu_idx_num) {
		gpu_freq_table= kzalloc(sizeof(struct cpufreq_frequency_table)
							* gpu_idx_num, GFP_KERNEL);
		if (!gpu_freq_table) {
			dev_err(&pdev->dev, "failed to allocate for gpu_table\n");
			return -ENODEV;
		}
		table_size = sizeof(struct cpufreq_frequency_table) / sizeof(unsigned int);

		ret = of_property_read_u32_array(pdev->dev.of_node, "gpu_cooling_table",
			(unsigned int *)gpu_freq_table, table_size * gpu_idx_num);

		for (i = 0; i < gpu_idx_num; i++)
			dev_info(&pdev->dev, "[GPU TMU] index : %d, frequency : %d \n",
				gpu_freq_table[i].driver_data, gpu_freq_table[i].frequency);
	}
#endif
	return ret;
}
EXPORT_SYMBOL_GPL(gpu_cooling_table_init);
