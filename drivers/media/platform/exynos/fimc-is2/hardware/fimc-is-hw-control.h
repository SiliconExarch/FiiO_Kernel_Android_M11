/*
 * Samsung EXYNOS FIMC-IS (Imaging Subsystem) driver
 *
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef FIMC_IS_DEVICE_HARDWARE_H
#define FIMC_IS_DEVICE_HARDWARE_H

#include <linux/videodev2.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include "fimc-is-config.h"
#include "fimc-is-framemgr.h"
#include "fimc-is-groupmgr.h"
#include "../fimc-is-interface.h"
#include "../include/fimc-is-hw.h"
#include "../include/exynos-fimc-is-sensor.h"
#include "fimc-is-err.h"

#define FIMC_IS_HW_STOP_TIMEOUT		(HZ / 4)
#define FIMC_IS_HW_CORE_END		(0x20141225) /* magic number */
#define FIMC_IS_MAX_HW_FRAME		(20)
#define FIMC_IS_MAX_HW_FRAME_LATE	(5)

#define DEBUG_FRAME_COUNT		3
#define DEBUG_POINT_HW_SHOT		0
#define DEBUG_POINT_FRAME_START		1
#define DEBUG_POINT_FRAME_END		2
#define DEBUG_POINT_FRAME_DMA_END	3
#define DEBUG_POINT_MAX			4

#define SET_FILE_MAGIC_NUMBER		(0x12345679)
#define FIMC_IS_MAX_SCENARIO		(64)
#define FIMC_IS_MAX_SETFILE		(64)
#define FIMC_IS_SHOT_TIMEOUT		(3000) /* ms */

#define SETFILE_DESIGN_BIT_3AA_ISP	(3)
#define SETFILE_DESIGN_BIT_DRC		(4)
#define SETFILE_DESIGN_BIT_SCC		(5)
#define SETFILE_DESIGN_BIT_ODC		(6)
#define SETFILE_DESIGN_BIT_VDIS		(7)
#define SETFILE_DESIGN_BIT_TDNR		(8)
#define SETFILE_DESIGN_BIT_SCX_MCSC	(9)
#define SETFILE_DESIGN_BIT_FD_VRA	(10)

#define REQ_FLAG(id)			(1 << (id))
#define OUT_FLAG(flag, subdev_id) (flag & ~(REQ_FLAG(subdev_id)))
#define check_hw_bug_count(this, count) \
	if (atomic_inc_return(&this->bug_count) > count) BUG_ON(1)

#define FIMC_IS_CLOCK_ON(addr, bit) \
	__raw_writel(__raw_readl((addr)) | (1 << (bit)), (addr))
#define FIMC_IS_CLOCK_OFF(addr, bit) \
	__raw_writel(__raw_readl((addr)) & ~(1 << (bit)), (addr))

/* sysfs variable for debug */
extern struct fimc_is_sysfs_debug sysfs_debug;

enum v_enum {
	V_BLANK = 0,
	V_VALID
};

enum fimc_is_hardware_id {
	DEV_HW_3AA0	= 1,
	DEV_HW_3AA1,
	DEV_HW_ISP0,
	DEV_HW_ISP1,
	DEV_HW_DRC,	/* = 5 */
	DEV_HW_SCC,
	DEV_HW_DIS,
	DEV_HW_3DNR,
	DEV_HW_TPU0,
	DEV_HW_TPU1,	/* = 10 */
	DEV_HW_SCP,
	DEV_HW_MCSC0,
	DEV_HW_MCSC1,
	DEV_HW_FD,
	DEV_HW_VRA,	/* = 15 */
	DEV_HW_DCP,
	DEV_HW_SRDZ,
	DEV_HW_END
};

/**
 * enum fimc_is_hw_state - the definition of HW state
 * @HW_OPEN : setbit @ open / clearbit @ close
 *            the upper layer is going to use this HW IP
 *            initialize frame manager for output or input frame control
 *            set by open stage and cleared by close stage
 *            multiple open is permitted but initialization is done
 *            at the first open only
 * @HW_INIT : setbit @ init / clearbit @ close
 *            define HW path at each instance
 *            the HW prepares context for this instance
 *            multiple init is premitted to support multi instance
 * @HW_CONFIG : setbit @ shot / clearbit @ frame start
 *              Update configuration parameters to apply each HW settings
 *              config operation must be done at least one time to run HW
 * @HW_RUN : setbit @ frame start / clearbit @ frame end
 *           running state of each HW
 *         OPEN --> INIT --> CONFIG ---> RUN
 *         | ^      | ^^     | ^           |
 *         |_|      |_||     |_|           |
 *                     |___________________|
 */
enum fimc_is_hw_state {
	HW_OPEN,
	HW_INIT,
	HW_CONFIG,
	HW_RUN,
	HW_TUNESET,
	HW_VRA_CH1_START,
	HW_END
};

enum fimc_is_shot_type {
	SHOT_TYPE_INTERNAL = 1,
	SHOT_TYPE_EXTERNAL,
	SHOT_TYPE_LATE,
	SHOT_TYPE_MULTI,
	SHOT_TYPE_END
};

enum fimc_is_setfile_type {
	SETFILE_V2 = 2,
	SETFILE_V3 = 3,
	SETFILE_MAX
};

struct hw_debug_info {
	u32			fcount;
	u32			cpuid[DEBUG_POINT_MAX];
	unsigned long long	time[DEBUG_POINT_MAX];
};

struct hw_ip_count{
	atomic_t		fs;
	atomic_t		cl;
	atomic_t		fe;
	atomic_t		dma;
};

struct hw_ip_status {
	atomic_t		otf_start;
	atomic_t		Vvalid;
	wait_queue_head_t	wait_queue;
};

/* file-mapped setfile header */
struct __setfile_header_ver_2 {
	u32	magic_number;
	u32	scenario_num;
	u32	subip_num;
	u32	setfile_offset;
} __attribute__((__packed__));

struct __setfile_header_ver_3 {
	u32	magic_number;
	u32	designed_bit;
	char	version_code[4];
	char	revision_code[4];
	u32	scenario_num;
	u32	subip_num;
	u32	setfile_offset;
} __attribute__((__packed__));

union __setfile_header {
	u32	magic_number;
	struct __setfile_header_ver_2 ver_2;
	struct __setfile_header_ver_3 ver_3;
} __attribute__((__packed__));

struct __setfile_table_entry {
	u32 offset;
	u32 size;
};

/* processed setfile header */
struct fimc_is_setfile_header {
	u32	version;

	u32	num_ips;
	u32	num_scenarios;

	ulong	scenario_table_base;	/* scenario : setfile index for each IP */
	ulong	num_setfile_base;	/* number of setfile for each IP */
	ulong	setfile_table_base;	/* setfile index : [offset, size] */
	ulong	setfile_entries_base;	/* actual setfile entries */

	/* extra information depend on the version */
	u32	designed_bits;
	char	version_code[5];
	char	revision_code[5];
};

struct setfile_table_entry {
	ulong addr;
	u32 size;
};

struct fimc_is_hw_ip_setfile {
	int				version;
	u32				applied_scenario;
	/* the number of setfile each sub ip has */
	u32				using_count;
	/* which subindex is used at this scenario */
	u32				index[FIMC_IS_MAX_SCENARIO];
	struct setfile_table_entry	table[FIMC_IS_MAX_SETFILE];
};

struct fimc_is_clk_gate {
	void __iomem			*regs;
	spinlock_t			slock;
	u32				bit[HW_SLOT_MAX];
	int				refcnt[HW_SLOT_MAX];
};

/**
 * struct fimc_is_hw - fimc-is hw data structure
 * @id: unique id to represent sub IP of IS chain
 * @state: HW state flags
 * @base_addr: Each IP mmaped registers region
 * @mcuctl_addr: MCUCTL mmaped registers region
 * @priv_info: the specific structure pointer for each HW IP
 * @group: pointer to indicate the HW IP belongs to the group
 * @region: pointer to parameter region for HW setting
 * @hindex: high-32bit index to indicate update field of parameter region
 * @lindex: low-32bit index to indicate update field of parameter region
 * @framemgr: pointer to frame manager to manager frame list HW handled
 * @hardware: pointer to device hardware
 * @itf: pointer to interface stucture to reply command
 * @itfc: pointer to chain interface for HW interrupt
 */
struct fimc_is_hw_ip {
	u32					id;
	char					name[FIMC_IS_STR_LEN];
	bool					is_leader;
	ulong					state;
	const struct fimc_is_hw_ip_ops		*ops;
	u32					debug_index[2];
	struct hw_debug_info			debug_info[DEBUG_FRAME_COUNT];
	struct hw_ip_count			count;
	struct hw_ip_status			status;
	atomic_t				fcount;
	atomic_t				instance;
	u32					internal_fcount;
	void __iomem				*regs;
	resource_size_t				regs_start;
	resource_size_t				regs_end;
	void __iomem				*regs_b;
	resource_size_t				regs_b_start;
	resource_size_t				regs_b_end;
	void					*priv_info;
	struct fimc_is_group			*group[FIMC_IS_STREAM_COUNT];
	struct is_region			*region[FIMC_IS_STREAM_COUNT];
	u32					hindex[FIMC_IS_STREAM_COUNT];
	u32					lindex[FIMC_IS_STREAM_COUNT];
	struct fimc_is_framemgr			*framemgr;
	struct fimc_is_framemgr			*framemgr_late;
	struct fimc_is_hardware			*hardware;
	/* callback interface */
	struct fimc_is_interface		*itf;
	/* control interface */
	struct fimc_is_interface_ischain	*itfc;
	struct fimc_is_hw_ip_setfile		setfile[SENSOR_POSITION_END];
	/* for dump sfr */
	u8					*sfr_dump;
	u8					*sfr_b_dump;
	atomic_t				rsccount;

	struct fimc_is_clk_gate			*clk_gate;
	u32					clk_gate_idx;

	struct timer_list			shot_timer;

	/* multi-buffer */
	struct fimc_is_frame			*mframe; /* CAUTION: read only */
	u32					num_buffers; /* total number of buffers per frame */
	u32					cur_s_int; /* count of start interrupt in multi-buffer */
	u32					cur_e_int; /* count of end interrupt in multi-buffer */
#if defined(MULTI_SHOT_TASKLET)
	struct tasklet_struct			tasklet_mshot;
#elif defined(MULTI_SHOT_KTHREAD)
	struct task_struct			*mshot_task;
	struct kthread_worker			mshot_worker;
	struct kthread_work 			mshot_work;
#endif
};

#define CALL_HW_OPS(hw, op, args...)	\
	(((hw)->ops->op) ? ((hw)->ops->op(hw, args)) : 0)

struct fimc_is_hw_ip_ops {
	int (*open)(struct fimc_is_hw_ip *hw_ip, u32 instance,
		struct fimc_is_group *group);
	int (*init)(struct fimc_is_hw_ip *hw_ip, u32 instance,
		struct fimc_is_group *group, bool flag, u32 module_id);
	int (*deinit)(struct fimc_is_hw_ip *hw_ip, u32 instance);
	int (*close)(struct fimc_is_hw_ip *hw_ip, u32 instance);
	int (*enable)(struct fimc_is_hw_ip *hw_ip, u32 instance, ulong hw_map);
	int (*disable)(struct fimc_is_hw_ip *hw_ip, u32 instance, ulong hw_map);
	int (*shot)(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame, ulong hw_map);
	int (*set_param)(struct fimc_is_hw_ip *hw_ip, struct is_region *region,
		u32 lindex, u32 hindex, u32 instance, ulong hw_map);
	int (*get_meta)(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame,
		ulong hw_map);
	int (*frame_ndone)(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame,
		u32 instance, enum ShotErrorType done_type);
	int (*load_setfile)(struct fimc_is_hw_ip *hw_ip, u32 instance, ulong hw_map);
	int (*apply_setfile)(struct fimc_is_hw_ip *hw_ip, u32 scenario, u32 instance,
		ulong hw_map);
	int (*delete_setfile)(struct fimc_is_hw_ip *hw_ip, u32 instance, ulong hw_map);
	void (*size_dump)(struct fimc_is_hw_ip *hw_ip);
	void (*clk_gate)(struct fimc_is_hw_ip *hw_ip, u32 instance, bool on, bool close);
};

/**
 * struct fimc_is_hardware - common HW chain structure
 * @taa0: 3AA0 HW IP structure
 * @taa1: 3AA1 HW IP structure
 * @isp0: ISP0 HW IP structure
 * @isp1: ISP1 HW IP structure
 * @drc: DRC HW IP structure
 * @scc: CODEC SCALER HW IP structure
 * @dis: VDIS HW IP structure
 * @tdnr: 3DNR HW IP structure
 * @scp: PREVIEW SCALER HW IP structure
 * @fd: LHFD HW IP structure
 * @framemgr: frame manager structure. each group has its own frame manager
 */
struct fimc_is_hardware {
	struct fimc_is_hw_ip		hw_ip[HW_SLOT_MAX];
	struct fimc_is_framemgr		framemgr[GROUP_ID_MAX];
	struct fimc_is_framemgr		framemgr_late[GROUP_ID_MAX];
	atomic_t			rsccount;

	/* keep last configuration */
	ulong				hw_map[FIMC_IS_STREAM_COUNT];
	u32				sensor_position[FIMC_IS_STREAM_COUNT];

	/* for access mcuctl regs */
	void __iomem			*base_addr_mcuctl;

	atomic_t			streaming[FIMC_IS_STREAM_COUNT];
	atomic_t			bug_count;
	atomic_t			log_count;
};

u32 get_hw_id_from_group(u32 group_id);
void fimc_is_hardware_flush_frame(struct fimc_is_hw_ip *hw_ip,
	enum fimc_is_hw_frame_state state,
	enum ShotErrorType done_type);
int fimc_is_hardware_probe(struct fimc_is_hardware *hardware,
	struct fimc_is_interface *itf, struct fimc_is_interface_ischain *itfc);
int fimc_is_hardware_set_param(struct fimc_is_hardware *hardware, u32 instance,
	struct is_region *region, u32 lindex, u32 hindex, ulong hw_map);
int fimc_is_hardware_shot(struct fimc_is_hardware *hardware, u32 instance,
	struct fimc_is_group *group, struct fimc_is_frame *frame,
	struct fimc_is_framemgr *framemgr, ulong hw_map, u32 framenum);
int fimc_is_hardware_grp_shot(struct fimc_is_hardware *hardware, u32 instance,
	struct fimc_is_group *group, struct fimc_is_frame *frame, ulong hw_map);
int fimc_is_hardware_config_lock(struct fimc_is_hw_ip *hw_ip, u32 instance, u32 framenum);
void fimc_is_hardware_frame_start(struct fimc_is_hw_ip *hw_ip, u32 instance);
int fimc_is_hardware_sensor_start(struct fimc_is_hardware *hardware, u32 instance,
	ulong hw_map);
int fimc_is_hardware_sensor_stop(struct fimc_is_hardware *hardware, u32 instance,
	ulong hw_map);
int fimc_is_hardware_process_start(struct fimc_is_hardware *hardware, u32 instance,
	u32 group_id);
void fimc_is_hardware_process_stop(struct fimc_is_hardware *hardware, u32 instance,
	u32 group_id, u32 mode);
int fimc_is_hardware_open(struct fimc_is_hardware *hardware, u32 hw_id,
	struct fimc_is_group *group, u32 instance, bool rep_flag, u32 module_id);
int fimc_is_hardware_close(struct fimc_is_hardware *hardware, u32 hw_id, u32 instance);
void fimc_is_hardware_clear(struct fimc_is_hardware *hardware);
int fimc_is_hardware_frame_done(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame,
	int wq_id, u32 output_id, enum ShotErrorType done_type);
int fimc_is_hardware_shot_done(struct fimc_is_hw_ip *hw_ip, struct fimc_is_frame *frame,
	struct fimc_is_framemgr *framemgr, enum ShotErrorType done_type);
int fimc_is_hardware_frame_ndone(struct fimc_is_hw_ip *ldr_hw_ip,
	struct fimc_is_frame *frame, u32 instance, enum ShotErrorType done_type);
int fimc_is_hardware_load_setfile(struct fimc_is_hardware *hardware, ulong addr,
	u32 instance, ulong hw_map);
int fimc_is_hardware_apply_setfile(struct fimc_is_hardware *hardware, u32 instance,
	u32 scenario, ulong hw_map);
int fimc_is_hardware_delete_setfile(struct fimc_is_hardware *hardware, u32 instance,
	ulong hw_map);
void fimc_is_hardware_size_dump(struct fimc_is_hw_ip *hw_ip);
void fimc_is_hardware_clk_gate_dump(struct fimc_is_hardware *hardware);
int fimc_is_hardware_runtime_resume(struct fimc_is_hardware *hardware);
int fimc_is_hardware_runtime_suspend(struct fimc_is_hardware *hardware);
void fimc_is_hardware_sfr_dump(struct fimc_is_hardware *hardware);
void print_all_hw_frame_count(struct fimc_is_hardware *hardware);
void fimc_is_hardware_clk_gate(struct fimc_is_hw_ip *hw_ip, u32 instance,
	bool on, bool close);
#endif
