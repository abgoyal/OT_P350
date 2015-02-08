/*
 * Synaptics RMI4 touchscreen driver
 *
 * Copyright (C) 2012 Synaptics Incorporated
 *
 * Copyright (C) 2012 Alexandra Chin <alexandra.chin@tw.synaptics.com>
 * Copyright (C) 2012 Scott Lin <scott.lin@tw.synaptics.com>
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
/*======================================================================================
 *                               	EDIT HISTORY FOR MOUDULE
 * This section contains comments describing changes made to the module.
 * Notice that  changes are listed in preverse charonological order.
 * when               who               what,where,why
 *-------------------------------------------------------------------------------------------------------------------
 *05/26/14      |weihong.chen   | add new feature,define below macro to enable the feature
 *                    |                      | double click to light up the screen:
 *                    |                      |       ----SYNAPTICS_GESTURE_WAKE_UP
 *                    |	                     | palm to light off the screen:
 *                    |                      |	   ----SYNAPTICS_PALM_SLEEP
 *                    |                      | show firmeare  version:
 *                    |                      |	   ----SYNAPTICS_VERSION
 *--------------------------------------------------------------------------------------------------------------------
========================================================================================*/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/input/synaptics_dsx.h>
#include <linux/of_gpio.h>
#include "synaptics_i2c_rmi4.h"
#include <linux/input/mt.h>
#ifdef  HAVE_HALL
#include "../misc/hall.h"
#endif
#define DRIVER_NAME "synaptics_rmi4_i2c"
#define INPUT_PHYS_NAME "synaptics_rmi4_i2c/input0"
#define DEBUGFS_DIR_NAME "ts_debug"

#define RESET_DELAY 100

#define TYPE_B_PROTOCOL

#define NO_0D_WHILE_2D
/*
#define REPORT_2D_Z
*/
#define REPORT_2D_W

#define RPT_TYPE (1 << 0)
#define RPT_X_LSB (1 << 1)
#define RPT_X_MSB (1 << 2)
#define RPT_Y_LSB (1 << 3)
#define RPT_Y_MSB (1 << 4)
#define RPT_Z (1 << 5)
#define RPT_WX (1 << 6)
#define RPT_WY (1 << 7)
#define RPT_DEFAULT (RPT_TYPE | RPT_X_LSB | RPT_X_MSB | RPT_Y_LSB | RPT_Y_MSB)

#define EXP_FN_DET_INTERVAL 1000 /* ms */
#define POLLING_PERIOD 1 /* ms */
#define SYN_I2C_RETRY_TIMES 10
#define MAX_ABS_MT_TOUCH_MAJOR 15

#define F01_STD_QUERY_LEN 21
#define F01_PACKAGE_ID_OFFSET 17
#define F01_BUID_ID_OFFSET 18
#define F11_STD_QUERY_LEN 9
#define F11_STD_CTRL_LEN 10
#define F11_STD_DATA_LEN 12

#define NORMAL_OPERATION 0
#define SENSOR_SLEEP 1
#define NO_SLEEP_OFF 0
#define NO_SLEEP_ON 1
//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, add new feature
#ifdef CONFIG_TCT_8X16_POP8LTE
#define SYNAPTICS_GESTURE_WAKE_UP
#define SYNAPTICS_PALM_SLEEP
#define SYNAPTICS_VERSION
#define TP_DEBUG
extern  u8 g_wakeup_gesture;
extern  u8 g_palm_lock_switch;
/* For Qualcomm 8916 I2C bulk read limitation */
#define I2C_LIMIT 255
struct i2c_msg *read_msg;
/**********************************************/

#endif
//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen, 2014/06/24, change  sensitive  when the hall is closed
#ifdef  HAVE_HALL
extern struct hall_data *hall;
static struct synaptics_rmi4_data *g_rmi4_data = NULL;
#endif
//[FEATURE]-Add-END by TCTSZ. weihong.chen, 2014/06/24, change  sensitive  when the hall is closed

#ifdef CONFIG_TCT_8X16_POP8LTE
static void synaptics_rmi4_set_configured(struct synaptics_rmi4_data *rmi4_data);
static void synaptics_hard_reset(struct synaptics_rmi4_data *rmi4_data)
{
    rmi4_data->irq_enable(rmi4_data,false);
	gpio_set_value(rmi4_data->board->reset_gpio, 1);
	msleep(1);
    gpio_set_value(rmi4_data->board->reset_gpio, 0);
	msleep(1);
    gpio_set_value(rmi4_data->board->reset_gpio, 1);   
	msleep(70);
    synaptics_rmi4_set_configured(rmi4_data);
    rmi4_data->irq_enable(rmi4_data,true);
	
 }
#endif

//gesture wakeup
#ifdef SYNAPTICS_GESTURE_WAKE_UP
#define F11_CONTINUOUS_MODE 0x00
#define F11_WAKEUP_GESTURE_MODE 0x04
#endif
/*ADD Begin by TCTSZ.weihong.chen,2014-6-30, Add a tp debug switch.*/
#ifdef TP_DEBUG
extern u8 g_tp_debug_on;

#define STP_DEBUG(fmt,arg...)          do{\
                                         if(g_tp_debug_on)\
                                         printk("<<-STP-DEBUG->> [%d]"fmt"\n",__LINE__, ##arg);\
                                       }while(0)

#define STP_DEBUG_FUNC()               do{\
                                         if(g_tp_debug_on)\
                                         printk("<<-STP-FUNC->> Func:%s@Line:%d\n",__func__,__LINE__);\
                                       }while(0)
#endif
/*ADD End by TCTSZ.weihong.chen,2014-6-30, Add a tp debug switch.*/
#if GTP_ESD_PROTECT
//static struct delayed_work gtp_esd_check_work;
static struct workqueue_struct * gtp_esd_check_workqueue = NULL;
static void gtp_esd_check_func(struct work_struct *);
void synaptics_esd_switch(struct synaptics_rmi4_data *rmi4_data, int on);
static void synaptics_hard_reset(struct synaptics_rmi4_data *rmi4_data);
#endif
#ifdef SYNAPTICS_GESTURE_WAKE_UP
static int synaptics_is_deep_suspended = 0;
#endif
#ifdef  SYNAPTICS_VERSION
extern char* g_tp_device_name;
extern int g_tp_firmware_ver;
extern int g_tp_cfg_ver ;
static int synaptics_read_firmware_version(struct synaptics_rmi4_data *rmi4_data);
static int synaptics_read_cfg_version(struct synaptics_rmi4_data *rmi4_data);
#endif
//gesture wakeup
#ifdef SYNAPTICS_GESTURE_WAKE_UP
static void synaptics_rmi4_f11_wg(struct synaptics_rmi4_data *rmi4_data,
		bool enable);
static ssize_t synaptics_rmi4_wake_gesture_show(struct device *dev,
		struct device_attribute *attr, char *buf);
static ssize_t synaptics_rmi4_wake_gesture_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);
		
static void synaptics_rmi4_wakeup_gesture(struct synaptics_rmi4_data *rmi4_data,
		bool enable);
#endif
/*
#ifdef SYNAPTICS_PALM_SLEEP
static ssize_t synaptics_rmi4_palm_gesture_show(struct device *dev,
		struct device_attribute *attr, char *buf);
static ssize_t synaptics_rmi4_palm_gesture_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);
#endif
*/
#ifdef SYNAPTICS_VERSION
static ssize_t synaptics_rmi4_firmware_version_show(struct device *dev,
		struct device_attribute *attr, char *buf);
#endif
//gesture wakeup
#ifdef SYNAPTICS_GESTURE_WAKE_UP
struct synaptics_rmi4_f11_query_0_5 {
	union {
		struct {
			/* query 0 */
			unsigned char f11_query0_b0__2:3;
			unsigned char has_query_9:1;
			unsigned char has_query_11:1;
			unsigned char has_query_12:1;
			unsigned char has_query_27:1;
			unsigned char has_query_28:1;

			/* query 1 */
			unsigned char num_of_fingers:3;
			unsigned char has_rel:1;
			unsigned char has_abs:1;
			unsigned char has_gestures:1;
			unsigned char has_sensitibity_adjust:1;
			unsigned char f11_query1_b7:1;

			/* query 2 */
			unsigned char num_of_x_electrodes;

			/* query 3 */
			unsigned char num_of_y_electrodes;

			/* query 4 */
			unsigned char max_electrodes:7;
			unsigned char f11_query4_b7:1;

			/* query 5 */
			unsigned char abs_data_size:2;
			unsigned char has_anchored_finger:1;
			unsigned char has_adj_hyst:1;
			unsigned char has_dribble:1;
			unsigned char has_bending_correction:1;
			unsigned char has_large_object_suppression:1;
			unsigned char has_jitter_filter:1;
		} __packed;
		unsigned char data[6];
	};
};

struct synaptics_rmi4_f11_query_7_8 {
	union {
		struct {
			/* query 7 */
			unsigned char has_single_tap:1;
			unsigned char has_tap_and_hold:1;
			unsigned char has_double_tap:1;
			unsigned char has_early_tap:1;
			unsigned char has_flick:1;
			unsigned char has_press:1;
			unsigned char has_pinch:1;
			unsigned char has_chiral_scroll:1;

			/* query 8 */
			unsigned char has_palm_detect:1;
			unsigned char has_rotate:1;
			unsigned char has_touch_shapes:1;
			unsigned char has_scroll_zones:1;
			unsigned char individual_scroll_zones:1;
			unsigned char has_multi_finger_scroll:1;
			unsigned char has_multi_finger_scroll_edge_motion:1;
			unsigned char has_multi_finger_scroll_inertia:1;
		} __packed;
		unsigned char data[2];
	};
};

struct synaptics_rmi4_f11_query_9 {
	union {
		struct {
			unsigned char has_pen:1;
			unsigned char has_proximity:1;
			unsigned char has_large_object_sensitivity:1;
			unsigned char has_suppress_on_large_object_detect:1;
			unsigned char has_two_pen_thresholds:1;
			unsigned char has_contact_geometry:1;
			unsigned char has_pen_hover_discrimination:1;
			unsigned char has_pen_hover_and_edge_filters:1;
		} __packed;
		unsigned char data[1];
	};
};

struct synaptics_rmi4_f11_query_12 {
	union {
		struct {
			unsigned char has_small_object_detection:1;
			unsigned char has_small_object_detection_tuning:1;
			unsigned char has_8bit_w:1;
			unsigned char has_2d_adjustable_mapping:1;
			unsigned char has_general_information_2:1;
			unsigned char has_physical_properties:1;
			unsigned char has_finger_limit:1;
			unsigned char has_linear_cofficient_2:1;
		} __packed;
		unsigned char data[1];
	};
};

struct synaptics_rmi4_f11_query_27 {
	union {
		struct {
			unsigned char f11_query27_b0:1;
			unsigned char has_pen_position_correction:1;
			unsigned char has_pen_jitter_filter_coefficient:1;
			unsigned char has_group_decomposition:1;
			unsigned char has_wakeup_gesture:1;
			unsigned char has_small_finger_correction:1;
			unsigned char has_data_37:1;
			unsigned char f11_query27_b7:1;
		} __packed;
		unsigned char data[1];
	};
};

struct synaptics_rmi4_f11_ctrl_6_9 {
	union {
		struct {
			unsigned char sensor_max_x_pos_7_0;
			unsigned char sensor_max_x_pos_11_8:4;
			unsigned char f11_ctrl7_b4__7:4;
			unsigned char sensor_max_y_pos_7_0;
			unsigned char sensor_max_y_pos_11_8:4;
			unsigned char f11_ctrl9_b4__7:4;
		} __packed;
		unsigned char data[4];
	};
};
#endif
//[FEATURE]-Add-END by TCTSZ.weihong.chen
//gw
enum device_status {
	STATUS_NO_ERROR = 0x00,
	STATUS_RESET_OCCURED = 0x01,
	STATUS_INVALID_CONFIG = 0x02,
	STATUS_DEVICE_FAILURE = 0x03,
	STATUS_CONFIG_CRC_FAILURE = 0x04,
	STATUS_FIRMWARE_CRC_FAILURE = 0x05,
	STATUS_CRC_IN_PROGRESS = 0x06,
	STATUS_UNCONFIGURED = 0x80
};

#define DEVICE_CONFIGURED 0x1

#define RMI4_VTG_MIN_UV		2700000
#define RMI4_VTG_MAX_UV		3300000
#define RMI4_ACTIVE_LOAD_UA	15000
#define RMI4_LPM_LOAD_UA	10

#define RMI4_I2C_VTG_MIN_UV	1800000
#define RMI4_I2C_VTG_MAX_UV	1800000
#define RMI4_I2C_LOAD_UA	10000
#define RMI4_I2C_LPM_LOAD_UA	10

#define RMI4_GPIO_SLEEP_LOW_US 10000
#define F12_FINGERS_TO_SUPPORT 10
#define MAX_F11_TOUCH_WIDTH 15

#define RMI4_COORDS_ARR_SIZE 4

#define F11_MAX_X		4096
#define F11_MAX_Y		4096
#define F12_MAX_X		65536
#define F12_MAX_Y		65536

static int synaptics_rmi4_i2c_read(struct synaptics_rmi4_data *rmi4_data,
		unsigned short addr, unsigned char *data,
		unsigned short length);

static int synaptics_rmi4_i2c_write(struct synaptics_rmi4_data *rmi4_data,
		unsigned short addr, unsigned char *data,
		unsigned short length);

static int synaptics_rmi4_reset_device(struct synaptics_rmi4_data *rmi4_data);

static void synaptics_rmi4_sensor_wake(struct synaptics_rmi4_data *rmi4_data);

static void __maybe_unused synaptics_rmi4_sensor_sleep(
			struct synaptics_rmi4_data *rmi4_data);

static int __maybe_unused synaptics_rmi4_regulator_lpm(
			struct synaptics_rmi4_data *rmi4_data, bool on);

static void __maybe_unused synaptics_rmi4_release_all(
			struct synaptics_rmi4_data *rmi4_data);

static int synaptics_rmi4_check_configuration(struct synaptics_rmi4_data
		*rmi4_data);

static int synaptics_rmi4_suspend(struct device *dev);

static int synaptics_rmi4_resume(struct device *dev);

static ssize_t synaptics_rmi4_full_pm_cycle_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t synaptics_rmi4_full_pm_cycle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);
//gw		
#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
static void synaptics_rmi4_early_suspend(struct early_suspend *h);

static void synaptics_rmi4_late_resume(struct early_suspend *h);
#endif

static ssize_t synaptics_rmi4_f01_reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t synaptics_rmi4_f01_productinfo_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t synaptics_rmi4_f01_buildid_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t synaptics_rmi4_f01_flashprog_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t synaptics_rmi4_0dbutton_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t synaptics_rmi4_0dbutton_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t synaptics_rmi4_flipx_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t synaptics_rmi4_flipx_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t synaptics_rmi4_flipy_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t synaptics_rmi4_flipy_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static int synaptics_rmi4_capacitance_button_map(
				struct synaptics_rmi4_data *rmi4_data,
				struct synaptics_rmi4_fn *fhandler);

struct synaptics_rmi4_f01_device_status {
	union {
		struct {
			unsigned char status_code:4;
			unsigned char reserved:2;
			unsigned char flash_prog:1;
			unsigned char unconfigured:1;
		} __packed;
		unsigned char data[1];
	};
};

struct synaptics_rmi4_f01_device_control_0 {
	union {
		struct {
			unsigned char sleep_mode:2;
			unsigned char nosleep:1;
			unsigned char reserved:2;
			unsigned char charger_input:1;
			unsigned char report_rate:1;
			unsigned char configured:1;
		} __packed;
		unsigned char data[1];
	};
};

//gw
struct synaptics_rmi4_f12_query_5 {
	union {
		struct {
			unsigned char size_of_query6;
			struct {
				unsigned char ctrl0_is_present:1;
				unsigned char ctrl1_is_present:1;
				unsigned char ctrl2_is_present:1;
				unsigned char ctrl3_is_present:1;
				unsigned char ctrl4_is_present:1;
				unsigned char ctrl5_is_present:1;
				unsigned char ctrl6_is_present:1;
				unsigned char ctrl7_is_present:1;
			} __packed;
			struct {
				unsigned char ctrl8_is_present:1;
				unsigned char ctrl9_is_present:1;
				unsigned char ctrl10_is_present:1;
				unsigned char ctrl11_is_present:1;
				unsigned char ctrl12_is_present:1;
				unsigned char ctrl13_is_present:1;
				unsigned char ctrl14_is_present:1;
				unsigned char ctrl15_is_present:1;
			} __packed;
			struct {
				unsigned char ctrl16_is_present:1;
				unsigned char ctrl17_is_present:1;
				unsigned char ctrl18_is_present:1;
				unsigned char ctrl19_is_present:1;
				unsigned char ctrl20_is_present:1;
				unsigned char ctrl21_is_present:1;
				unsigned char ctrl22_is_present:1;
				unsigned char ctrl23_is_present:1;
			} __packed;
			struct {
				unsigned char ctrl24_is_present:1;
				unsigned char ctrl25_is_present:1;
				unsigned char ctrl26_is_present:1;
				unsigned char ctrl27_is_present:1;
				unsigned char ctrl28_is_present:1;
				unsigned char ctrl29_is_present:1;
				unsigned char ctrl30_is_present:1;
				unsigned char ctrl31_is_present:1;
			} __packed;
		};
		unsigned char data[5];
	};
};

struct synaptics_rmi4_f12_query_8 {
	union {
		struct {
			unsigned char size_of_query9;
			struct {
				unsigned char data0_is_present:1;
				unsigned char data1_is_present:1;
				unsigned char data2_is_present:1;
				unsigned char data3_is_present:1;
				unsigned char data4_is_present:1;
				unsigned char data5_is_present:1;
				unsigned char data6_is_present:1;
				unsigned char data7_is_present:1;
			} __packed;
			struct {
				unsigned char data8_is_present:1;
				unsigned char data9_is_present:1;
				unsigned char data10_is_present:1;
				unsigned char data11_is_present:1;
				unsigned char data12_is_present:1;
				unsigned char data13_is_present:1;
				unsigned char data14_is_present:1;
				unsigned char data15_is_present:1;
			} __packed;
		};
		unsigned char data[3];
	};
};

struct synaptics_rmi4_f12_ctrl_8 {
	union {
		struct {
			unsigned char max_x_coord_lsb;
			unsigned char max_x_coord_msb;
			unsigned char max_y_coord_lsb;
			unsigned char max_y_coord_msb;
			unsigned char rx_pitch_lsb;
			unsigned char rx_pitch_msb;
			unsigned char tx_pitch_lsb;
			unsigned char tx_pitch_msb;
			unsigned char low_rx_clip;
			unsigned char high_rx_clip;
			unsigned char low_tx_clip;
			unsigned char high_tx_clip;
			unsigned char num_of_rx;
			unsigned char num_of_tx;
		};
		unsigned char data[14];
	};
};

struct synaptics_rmi4_f12_ctrl_23 {
	union {
		struct {
			unsigned char obj_type_enable;
			unsigned char max_reported_objects;
		};
		unsigned char data[2];
	};
};

struct synaptics_rmi4_f12_finger_data {
	unsigned char object_type_and_status;
	unsigned char x_lsb;
	unsigned char x_msb;
	unsigned char y_lsb;
	unsigned char y_msb;
#ifdef REPORT_2D_Z
	unsigned char z;
#endif
#ifdef REPORT_2D_W
	unsigned char wx;
	unsigned char wy;
#endif
};

struct synaptics_rmi4_f1a_query {
	union {
		struct {
			unsigned char max_button_count:3;
			unsigned char reserved:5;
			unsigned char has_general_control:1;
			unsigned char has_interrupt_enable:1;
			unsigned char has_multibutton_select:1;
			unsigned char has_tx_rx_map:1;
			unsigned char has_perbutton_threshold:1;
			unsigned char has_release_threshold:1;
			unsigned char has_strongestbtn_hysteresis:1;
			unsigned char has_filter_strength:1;
		} __packed;
		unsigned char data[2];
	};
};

struct synaptics_rmi4_f1a_control_0 {
	union {
		struct {
			unsigned char multibutton_report:2;
			unsigned char filter_mode:2;
			unsigned char reserved:4;
		} __packed;
		unsigned char data[1];
	};
};

struct synaptics_rmi4_f1a_control_3_4 {
	unsigned char transmitterbutton;
	unsigned char receiverbutton;
};

struct synaptics_rmi4_f1a_control {
	struct synaptics_rmi4_f1a_control_0 general_control;
	unsigned char *button_int_enable;
	unsigned char *multi_button;
	struct synaptics_rmi4_f1a_control_3_4 *electrode_map;
	unsigned char *button_threshold;
	unsigned char button_release_threshold;
	unsigned char strongest_button_hysteresis;
	unsigned char filter_strength;
};

struct synaptics_rmi4_f1a_handle {
	int button_bitmask_size;
	unsigned char button_count;
	unsigned char valid_button_count;
	unsigned char *button_data_buffer;
	unsigned char *button_map;
	struct synaptics_rmi4_f1a_query button_query;
	struct synaptics_rmi4_f1a_control button_control;
};

struct synaptics_rmi4_f12_extra_data {
	unsigned char data1_offset;
	unsigned char data15_offset;
	unsigned char data15_size;
	unsigned char data15_data[(F12_FINGERS_TO_SUPPORT + 7) / 8];
};

struct synaptics_rmi4_exp_fn {
	enum exp_fn fn_type;
	bool inserted;
	int (*func_init)(struct synaptics_rmi4_data *rmi4_data);
	void (*func_remove)(struct synaptics_rmi4_data *rmi4_data);
	void (*func_attn)(struct synaptics_rmi4_data *rmi4_data,
			unsigned char intr_mask);
	struct list_head link;
};

static struct device_attribute attrs[] = {
	__ATTR(full_pm_cycle, (S_IRUGO | S_IWUSR | S_IWGRP),
			synaptics_rmi4_full_pm_cycle_show,
			synaptics_rmi4_full_pm_cycle_store),
	__ATTR(reset, S_IWUSR | S_IWGRP,
			NULL,
			synaptics_rmi4_f01_reset_store),
	__ATTR(productinfo, S_IRUGO,
			synaptics_rmi4_f01_productinfo_show,
			synaptics_rmi4_store_error),
	__ATTR(buildid, S_IRUGO,
			synaptics_rmi4_f01_buildid_show,
			synaptics_rmi4_store_error),
	__ATTR(flashprog, S_IRUGO,
			synaptics_rmi4_f01_flashprog_show,
			synaptics_rmi4_store_error),
	__ATTR(0dbutton, (S_IRUGO | S_IWUSR | S_IWGRP),
			synaptics_rmi4_0dbutton_show,
			synaptics_rmi4_0dbutton_store),
	__ATTR(flipx, (S_IRUGO | S_IWUSR | S_IWGRP),
			synaptics_rmi4_flipx_show,
			synaptics_rmi4_flipx_store),
	__ATTR(flipy, (S_IRUGO | S_IWUSR | S_IWGRP),
			synaptics_rmi4_flipy_show,
			synaptics_rmi4_flipy_store),
//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, add new feature
    #ifdef SYNAPTICS_GESTURE_WAKE_UP
	__ATTR(wakeup_gesture, (S_IRUGO | S_IWUGO),
			synaptics_rmi4_wake_gesture_show,
			synaptics_rmi4_wake_gesture_store),
    #endif
	/*
    #ifdef SYNAPTICS_PALM_SLEEP
	__ATTR(palm_gesture, (S_IRUGO | S_IWUGO),
			synaptics_rmi4_palm_gesture_show,
			synaptics_rmi4_palm_gesture_store),
	#endif
	*/
	#ifdef  SYNAPTICS_VERSION
	__ATTR(firmware_version, S_IRUGO ,
			synaptics_rmi4_firmware_version_show,
	         NULL),
	#endif
//[FEATURE]-Add-END by TCTSZ.weihong.chen
};

static bool exp_fn_inited;
static struct mutex exp_fn_list_mutex;
static struct list_head exp_fn_list;
//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, add new feature

#ifdef  SYNAPTICS_VERSION
static unsigned int version_extract_uint(const unsigned char *ptr)
 {
	 return (unsigned int)ptr[0] +
			 (unsigned int)ptr[1] * 0x100 +
			 (unsigned int)ptr[2] * 0x10000 +
			 (unsigned int)ptr[3] * 0x1000000;
 }
 static unsigned int extract_uint_be(const unsigned char *ptr)
 {
	 return (unsigned int)ptr[3] +
			 (unsigned int)ptr[2] * 0x100 +
			 (unsigned int)ptr[1] * 0x10000 +
			 (unsigned int)ptr[0] * 0x1000000;
 }

 static int synaptics_read_firmware_version(struct synaptics_rmi4_data *rmi4_data)
 {
   int retval;
   unsigned char firmware_id[4];
   if(synaptics_is_deep_suspended  || rmi4_data->fw_updating)
   	 return 0;
   retval = synaptics_rmi4_i2c_read(rmi4_data,
				   0x00a4,
				   firmware_id,
				sizeof(firmware_id));
    firmware_id[3] = 0;
    retval = version_extract_uint(firmware_id);
	return retval;
  }
 
 //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen 2014/06/16, add show cfg file version
 static int synaptics_read_cfg_version(struct synaptics_rmi4_data *rmi4_data)
 {
   int retval;
   unsigned char firmware_id[4];
   if(synaptics_is_deep_suspended  || rmi4_data->fw_updating)
   	 return 0;
   retval = synaptics_rmi4_i2c_read(rmi4_data,
				   0x004d,/*mod by weihong.chen ,07/03/2014 ,it should be 0x004d  in 1662965 firmware*/
				   firmware_id,
				sizeof(firmware_id));
    retval = extract_uint_be(firmware_id);
	return retval;
  }
 //[FEATURE]-Add-END by TCTSZ. weihong.chen 2014/06/16, add show cfg file version
#endif

//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen, 2014/06/24, change  sensitive  when the hall is closed
#ifdef  HAVE_HALL
#define  F51_CUSTOM_CTRL10  0x040b
#define  F51_CUSTOM_CTRL00  0X0401
void tp_set_sensitivity(int  enable)
{
   int retval=0;
   unsigned char f51_custom_ctrl10  =0;
   unsigned char f51_custom_ctrl00 =0;
    if(g_rmi4_data != NULL)
         if(!g_rmi4_data->fw_updating)
          {
            if(!enable)	//hall closed
			{  
			#ifdef TP_DEBUG
			STP_DEBUG("Cover closed,Set high sensitivity \n");
			#endif
				 f51_custom_ctrl10 = 0xfe;
				 f51_custom_ctrl00 =0x03;
			
			}
			else
			{ 
                        #ifdef TP_DEBUG
                        STP_DEBUG("Cover open,Set normal sensitivity \n");
			#endif
				 f51_custom_ctrl10 = 0x72;
				 f51_custom_ctrl00 =0x00;
			}
			retval = synaptics_rmi4_i2c_write(g_rmi4_data,
			F51_CUSTOM_CTRL00,
			&f51_custom_ctrl00,
			sizeof(f51_custom_ctrl00));
    	
			if (retval < 0) {
				dev_err(&g_rmi4_data->i2c_client->dev,
						"%s: Failed to change reporting mode\n",
	                        __func__);
		    	}

		    retval = synaptics_rmi4_i2c_write(g_rmi4_data,
			F51_CUSTOM_CTRL10,
			&f51_custom_ctrl10,
			sizeof(f51_custom_ctrl10));
    	
			if (retval < 0) {
				dev_err(&g_rmi4_data->i2c_client->dev,
						"%s: Failed to change reporting mode\n",
	                       __func__);
		    	}
    	}
}
#endif
//[FEATURE]-Add-END by TCTSZ. weihong.chen, 2014/06/24, change  sensitive  when the hall is closed
#ifdef CONFIG_TCT_8X16_POP8LTE
#define NO_SLEEP_ON_MODE (1 << 2)
#define CONFIGURED (1 << 7)

static void synaptics_rmi4_set_configured(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
	unsigned char device_ctrl;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			&device_ctrl,
			sizeof(device_ctrl));
	if (retval < 0) {
		//dev_err(&(rmi4_data->input_dev->dev),
		//		"%s: Failed to set configured\n",
		//		__func__);
		printk(KERN_ERR "==s3508 i2c==,%s,%d: Failed to set configured\n",__func__,
__LINE__);
		return;
	}

	rmi4_data->no_sleep_setting = device_ctrl & NO_SLEEP_ON_MODE;
	device_ctrl |= CONFIGURED;

	retval = synaptics_rmi4_i2c_write(rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			&device_ctrl,
			sizeof(device_ctrl));
	if (retval < 0) {
		//dev_err(&(rmi4_data->input_dev->dev),
		//		"%s: Failed to set configured\n",
		//		__func__);
		printk(KERN_ERR "==s3508 i2c==,%s,%d: Failed to set configured\n",__func__,
__LINE__);
	}
	return;
}
#endif

//gesture wakeup
#ifdef SYNAPTICS_GESTURE_WAKE_UP
#if 0
static void synaptics_rmi4_soft_reset(struct synaptics_rmi4_data *rmi4_data) 
{   int retval =0;
    unsigned char command =0x01;
	//[BUGFIX]-Add-BEGIN by TCTSZ. weihong.chen, PR753189,2014/08/04, reset all the register
	retval = synaptics_rmi4_i2c_write(rmi4_data,
			rmi4_data->f01_cmd_base_addr,
			&command,
			sizeof(command));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to issue reset command, error = %d\n",
				__func__, retval);

	    }

	 msleep(rmi4_data->board->reset_delay);
	 //[BUGFIX]-Add-END by TCTSZ. weihong.chen, 2014/08/04,PR753189, reset all the register
}

#endif
static void synaptics_rmi4_wakeup_gesture(struct synaptics_rmi4_data *rmi4_data,
		bool enable)
{
	if (rmi4_data->f11_wakeup_gesture)
		synaptics_rmi4_f11_wg(rmi4_data, enable);
	//else if (rmi4_data->f12_wakeup_gesture)
//		synaptics_rmi4_f12_wg(rmi4_data, enable);

	return;
}

static void synaptics_rmi4_f11_wg(struct synaptics_rmi4_data *rmi4_data,
		bool enable)
{
	int retval;
	unsigned char reporting_control;
	struct synaptics_rmi4_fn *fhandler;
	struct synaptics_rmi4_device_info *rmi;

	rmi = &(rmi4_data->rmi4_mod_info);

	list_for_each_entry(fhandler, &rmi->support_fn_list, link) {
		if (fhandler->fn_number == SYNAPTICS_RMI4_F11)
			break;
	}

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.ctrl_base,
			&reporting_control,
			sizeof(reporting_control));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to change reporting mode\n",
				__func__);
		return;
	}

	reporting_control = (reporting_control & ~MASK_3BIT);
	if (enable)
		{
		reporting_control |= F11_WAKEUP_GESTURE_MODE;
		rmi4_data->in_wakeup_gesture_mode =1;
		}
	else
		{
		reporting_control |= F11_CONTINUOUS_MODE;
		rmi4_data->in_wakeup_gesture_mode =0;
		}
	retval = synaptics_rmi4_i2c_write(rmi4_data,
			fhandler->full_addr.ctrl_base,
			&reporting_control,
			sizeof(reporting_control));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to change reporting mode\n",
				__func__);
		return;
	}
    
	return;
}

static ssize_t synaptics_rmi4_wake_gesture_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	   return snprintf(buf, PAGE_SIZE, "%u\n",
			g_wakeup_gesture);
}


static ssize_t synaptics_rmi4_wake_gesture_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;

	input = input > 0 ? 1 : 0;

	if (rmi4_data->f11_wakeup_gesture /*|| rmi4_data->f12_wakeup_gesture*/)
		g_wakeup_gesture= input;

	return count;
}
  #endif
/*
#ifdef SYNAPTICS_PALM_SLEEP
  static ssize_t synaptics_rmi4_palm_gesture_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n",
			rmi4_data->enable_palm_gesture);
}


static ssize_t synaptics_rmi4_palm_gesture_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;
    input = input > 0 ? 1 : 0;
    rmi4_data->enable_palm_gesture= input;

	return count;
}
#endif
*/

#if GTP_ESD_PROTECT
  /*******************************************************
  Function:
	  switch on & off esd delayed work
  Input:
	  client:  i2c device
	  on: SWITCH_ON / SWITCH_OFF
  Output:
	  void
  *********************************************************/
  void synaptics_esd_switch(struct synaptics_rmi4_data *rmi4_data, int on)
  {
	
      if (SWITCH_ON == on) {
		  /* switch on esd	*/
		  if (!rmi4_data->esd_running) {
			  rmi4_data->esd_running = 1;
			 // dev_dbg(&client->dev, "Esd started\n");
			// printk(KERN_CRIT"Esd started\n");
			  queue_delayed_work(gtp_esd_check_workqueue,
				  &rmi4_data->gtp_esd_check_work, GTP_ESD_CHECK_CIRCLE);
		  }
	  } else {
		  /* switch off esd */
		  if (rmi4_data->esd_running) {
			  rmi4_data->esd_running = 0;
			 // dev_dbg(&client->dev, "Esd cancelled\n");
			 // printk(KERN_CRIT"Esd stop\n");
			  cancel_delayed_work_sync(&rmi4_data->gtp_esd_check_work);
		  }
	  }
  }


static void gtp_esd_check_func(struct work_struct *work)
{   
    unsigned char esd_state =0;
	int retval =0;
    struct synaptics_rmi4_data *rmi4_data =
			container_of(work, struct synaptics_rmi4_data,
			gtp_esd_check_work.work);
	 #ifdef TP_DEBUG
	 STP_DEBUG("synaptics_esd_check \n");
	 #endif
     //printk(KERN_CRIT"synaptics_esd_check \n");
     if(rmi4_data->fw_updating)
     	{
             #ifdef TP_DEBUG
			 STP_DEBUG("firmware is updating ... exit esd check\n");
	         #endif
			 goto exit_fw_updating;
	    }
	 retval = synaptics_rmi4_i2c_read(rmi4_data,
				   ESD_ADDRESS,
				   &esd_state,
				sizeof(esd_state));
	 	if (retval < 0) {
			         esd_state =0x03;
		             dev_err(&rmi4_data->i2c_client->dev,
				     "%s: Failed to read esd state, error = %d\n",
				      __func__, retval);

	                   }
	  #ifdef TP_DEBUG
	  STP_DEBUG("address =0x%x,esd_state =0x%x \n",ESD_ADDRESS,esd_state);
	  #endif
     //  printk(KERN_CRIT"address =0x%x,esd_state =0x%x \n",ESD_ADDRESS,esd_state);
		if((esd_state&ESD_DATA_MASK) == 0x03)
			{
               //esd crash,need recovery
               // printk(KERN_CRIT"synaptics esd recovery \n");
               #ifdef TP_DEBUG
	           STP_DEBUG("synaptics esd recovery \n");
	           #endif
               synaptics_hard_reset(rmi4_data);
		    }

exit_fw_updating:		
    if(!rmi4_data->suspended)
    {
        queue_delayed_work(gtp_esd_check_workqueue, &rmi4_data->gtp_esd_check_work, rmi4_data->clk_tick_cnt);
    }
    
    return;
}

#endif
#ifdef SYNAPTICS_VERSION
static ssize_t synaptics_rmi4_firmware_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);
	
	return snprintf(buf, PAGE_SIZE, "synaptics:%u\n",
				synaptics_read_firmware_version(rmi4_data));
}
#endif
//[FEATURE]-Add-END by TCTSZ.weihong.chen

static int synaptics_rmi4_debug_suspend_set(void *_data, u64 val)
{
	struct synaptics_rmi4_data *rmi4_data = _data;

	if (val)
		synaptics_rmi4_suspend(&rmi4_data->input_dev->dev);
	else
		synaptics_rmi4_resume(&rmi4_data->input_dev->dev);

	return 0;
}

static int synaptics_rmi4_debug_suspend_get(void *_data, u64 *val)
{
	struct synaptics_rmi4_data *rmi4_data = _data;

	*val = rmi4_data->suspended;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_suspend_fops, synaptics_rmi4_debug_suspend_get,
			synaptics_rmi4_debug_suspend_set, "%lld\n");

static ssize_t synaptics_rmi4_full_pm_cycle_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n",
			rmi4_data->full_pm_cycle);
}

static ssize_t synaptics_rmi4_full_pm_cycle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;

	rmi4_data->full_pm_cycle = input > 0 ? 1 : 0;

	return count;
}

#ifdef CONFIG_FB
static void configure_sleep(struct synaptics_rmi4_data *rmi4_data)
{
	int retval = 0;

	rmi4_data->fb_notif.notifier_call = fb_notifier_callback;

	retval = fb_register_client(&rmi4_data->fb_notif);
	if (retval)
		dev_err(&rmi4_data->i2c_client->dev,
			"Unable to register fb_notifier: %d\n", retval);
	return;
}
#elif defined CONFIG_HAS_EARLYSUSPEND
static void configure_sleep(struct synaptics_rmi4_data *rmi4_data)
{
	rmi4_data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	rmi4_data->early_suspend.suspend = synaptics_rmi4_early_suspend;
	rmi4_data->early_suspend.resume = synaptics_rmi4_late_resume;
	register_early_suspend(&rmi4_data->early_suspend);

	return;
}
#else
static void configure_sleep(struct synaptics_rmi4_data *rmi4_data)
{
	return;
}
#endif

static ssize_t synaptics_rmi4_f01_reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	unsigned int reset;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	if (sscanf(buf, "%u", &reset) != 1)
		return -EINVAL;

	if (reset != 1)
		return -EINVAL;

	retval = synaptics_rmi4_reset_device(rmi4_data);
	if (retval < 0) {
		dev_err(dev,
			"%s: Failed to issue reset command, error = %d\n",
			__func__, retval);
		return retval;
	}

	return count;
}

static ssize_t synaptics_rmi4_f01_productinfo_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "0x%02x 0x%02x\n",
			(rmi4_data->rmi4_mod_info.product_info[0]),
			(rmi4_data->rmi4_mod_info.product_info[1]));
}

static ssize_t synaptics_rmi4_f01_buildid_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned int build_id;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);
	struct synaptics_rmi4_device_info *rmi;

	rmi = &(rmi4_data->rmi4_mod_info);

	build_id = (unsigned int)rmi->build_id[0] +
			(unsigned int)rmi->build_id[1] * 0x100 +
			(unsigned int)rmi->build_id[2] * 0x10000;

	return snprintf(buf, PAGE_SIZE, "%u\n",
			build_id);
}

static ssize_t synaptics_rmi4_f01_flashprog_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int retval;
	struct synaptics_rmi4_f01_device_status device_status;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_data_base_addr,
			device_status.data,
			sizeof(device_status.data));
	if (retval < 0) {
		dev_err(dev,
				"%s: Failed to read device status, error = %d\n",
				__func__, retval);
		return retval;
	}

	return snprintf(buf, PAGE_SIZE, "%u\n",
			device_status.flash_prog);
}

static ssize_t synaptics_rmi4_0dbutton_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n",
			rmi4_data->button_0d_enabled);
}

static ssize_t synaptics_rmi4_0dbutton_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int retval;
	unsigned int input;
	unsigned char ii;
	unsigned char intr_enable;
	struct synaptics_rmi4_fn *fhandler;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);
	struct synaptics_rmi4_device_info *rmi;

	rmi = &(rmi4_data->rmi4_mod_info);

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;

	input = input > 0 ? 1 : 0;

	if (rmi4_data->button_0d_enabled == input)
		return count;

	mutex_lock(&rmi->support_fn_list_mutex);
	if (!list_empty(&rmi->support_fn_list)) {
		list_for_each_entry(fhandler, &rmi->support_fn_list, link) {
			if (fhandler->fn_number == SYNAPTICS_RMI4_F1A) {
				ii = fhandler->intr_reg_num;

				retval = synaptics_rmi4_i2c_read(rmi4_data,
						rmi4_data->f01_ctrl_base_addr +
						1 + ii,
						&intr_enable,
						sizeof(intr_enable));
				if (retval < 0)
					goto exit;

				if (input == 1)
					intr_enable |= fhandler->intr_mask;
				else
					intr_enable &= ~fhandler->intr_mask;

				retval = synaptics_rmi4_i2c_write(rmi4_data,
						rmi4_data->f01_ctrl_base_addr +
						1 + ii,
						&intr_enable,
						sizeof(intr_enable));
				if (retval < 0)
					goto exit;
			}
		}
	}
	mutex_unlock(&rmi->support_fn_list_mutex);
	rmi4_data->button_0d_enabled = input;

	return count;
exit:
	mutex_unlock(&rmi->support_fn_list_mutex);
	return retval;
}

static ssize_t synaptics_rmi4_flipx_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n",
		rmi4_data->flip_x);
}

static ssize_t synaptics_rmi4_flipx_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;

	rmi4_data->flip_x = input > 0 ? 1 : 0;

	return count;
}

static ssize_t synaptics_rmi4_flipy_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n",
		rmi4_data->flip_y);
}

static ssize_t synaptics_rmi4_flipy_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;

	rmi4_data->flip_y = input > 0 ? 1 : 0;

	return count;
}

 /**
 * synaptics_rmi4_set_page()
 *
 * Called by synaptics_rmi4_i2c_read() and synaptics_rmi4_i2c_write().
 *
 * This function writes to the page select register to switch to the
 * assigned page.
 */
static int synaptics_rmi4_set_page(struct synaptics_rmi4_data *rmi4_data,
		unsigned int address)
{
	int retval = 0;
	unsigned char retry;
	unsigned char buf[PAGE_SELECT_LEN];
	unsigned char page;
	struct i2c_client *i2c = rmi4_data->i2c_client;

	page = ((address >> 8) & MASK_8BIT);
	if (page != rmi4_data->current_page) {
		buf[0] = MASK_8BIT;
		buf[1] = page;
		for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
			retval = i2c_master_send(i2c, buf, PAGE_SELECT_LEN);
			if (retval != PAGE_SELECT_LEN) {
				dev_err(&i2c->dev,
						"%s: I2C retry %d\n",
						__func__, retry + 1);
				msleep(20);
			} else {
				rmi4_data->current_page = page;
				break;
			}
		}
	} else
		return PAGE_SELECT_LEN;
	return (retval == PAGE_SELECT_LEN) ? retval : -EIO;
}

 /**
 * synaptics_rmi4_i2c_read()
 *
 * Called by various functions in this driver, and also exported to
 * other expansion Function modules such as rmi_dev.
 *
 * This function reads data of an arbitrary length from the sensor,
 * starting from an assigned register address of the sensor, via I2C
 * with a retry mechanism.
 */
static int synaptics_rmi4_i2c_read(struct synaptics_rmi4_data *rmi4_data,
		unsigned short addr, unsigned char *data, unsigned short length)
{
	int retval;
	unsigned char retry;
	unsigned char buf;
	//[FEATURE]-MOD-BEGIN by TCTSZ. weihong.chen, 2014/07/03, fix it qcom IIC read length limit ,when read more than 255 bytes
	#ifdef CONFIG_TCT_8X16_POP8LTE
	unsigned int full = length / I2C_LIMIT; 
	unsigned int partial = length % I2C_LIMIT;
	unsigned int total;
	unsigned int last;
	int ii;
        static int msg_length =0;	
	if ( (full + 2) > msg_length ){
		kfree(read_msg);
		msg_length = full + 2;
		read_msg = kcalloc(msg_length, sizeof(struct i2c_msg), GFP_KERNEL);
	}

	read_msg[0].addr = rmi4_data->i2c_client->addr;
	read_msg[0].flags = 0;
	read_msg[0].len = 1;
	read_msg[0].buf = &buf;

	if (partial) {
		total = full + 1;
		last = partial;
	} else {
		total = full;
		last = I2C_LIMIT;
	}

	for ( ii = 1; ii <= total; ii++) {
		read_msg[ii].addr = rmi4_data->i2c_client->addr;
		read_msg[ii].flags = I2C_M_RD;
		read_msg[ii].len = ( ii == total ) ? last : I2C_LIMIT;
		read_msg[ii].buf = data + I2C_LIMIT * (ii - 1);
	}
        #else	
	struct i2c_msg msg[] = {
		{
			.addr = rmi4_data->i2c_client->addr,
			.flags = 0,
			.len = 1,
			.buf = &buf,
		},
		{
			.addr = rmi4_data->i2c_client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		},
	};
        #endif
        //[FEATURE]-MOD-END by TCTSZ. weihong.chen, 2014/07/03, fix it qcom IIC read length limit ,when read more than 255 bytes
	buf = addr & MASK_8BIT;

	mutex_lock(&(rmi4_data->rmi4_io_ctrl_mutex));

	retval = synaptics_rmi4_set_page(rmi4_data, addr);
	if (retval != PAGE_SELECT_LEN)
		goto exit;

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		//[FEATURE]-MOD-BEGIN by TCTSZ. weihong.chen, 2014/07/03, fix it qcom IIC read length limit ,when read more than 255 bytes
	        #ifdef CONFIG_TCT_8X16_POP8LTE
		if (i2c_transfer(rmi4_data->i2c_client->adapter, read_msg, (total + 1)) == (total + 1)) {
		#else
		if (i2c_transfer(rmi4_data->i2c_client->adapter, msg, 2) == 2) {
		#endif
		//[FEATURE]-MOD-END by TCTSZ. weihong.chen, 2014/07/03, fix it qcom IIC read length limit ,when read more than 255 bytes
			retval = length;
			break;
		}
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: I2C retry %d\n",
				__func__, retry + 1);
		msleep(20);
	}

	if (retry == SYN_I2C_RETRY_TIMES) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: I2C read over retry limit\n",
				__func__);
		retval = -EIO;
	}

exit:
	mutex_unlock(&(rmi4_data->rmi4_io_ctrl_mutex));

	return retval;
}

 /**
 * synaptics_rmi4_i2c_write()
 *
 * Called by various functions in this driver, and also exported to
 * other expansion Function modules such as rmi_dev.
 *
 * This function writes data of an arbitrary length to the sensor,
 * starting from an assigned register address of the sensor, via I2C with
 * a retry mechanism.
 */
static int synaptics_rmi4_i2c_write(struct synaptics_rmi4_data *rmi4_data,
		unsigned short addr, unsigned char *data, unsigned short length)
{
	int retval;
	unsigned char retry;
	unsigned char buf[length + 1];
	struct i2c_msg msg[] = {
		{
			.addr = rmi4_data->i2c_client->addr,
			.flags = 0,
			.len = length + 1,
			.buf = buf,
		}
	};

	mutex_lock(&(rmi4_data->rmi4_io_ctrl_mutex));

	retval = synaptics_rmi4_set_page(rmi4_data, addr);
	if (retval != PAGE_SELECT_LEN)
		goto exit;

	buf[0] = addr & MASK_8BIT;
	memcpy(&buf[1], &data[0], length);

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(rmi4_data->i2c_client->adapter, msg, 1) == 1) {
			retval = length;
			break;
		}
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: I2C retry %d\n",
				__func__, retry + 1);
		msleep(20);
	}

	if (retry == SYN_I2C_RETRY_TIMES) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: I2C write over retry limit\n",
				__func__);
		retval = -EIO;
	}

exit:
	mutex_unlock(&(rmi4_data->rmi4_io_ctrl_mutex));

	return retval;
}

/**
 * synaptics_rmi4_release_all()
 *
 * Called by synaptics_rmi4_suspend()
 *
 * Release all touch data during the touch device switch to suspend state.
 */

static void synaptics_rmi4_release_all(struct synaptics_rmi4_data *rmi4_data)
{
	int finger;
	int max_num_fingers = rmi4_data->num_of_fingers;
        #ifdef TP_DEBUG
        STP_DEBUG_FUNC();
	#endif
     for (finger = 0; finger < max_num_fingers; finger++) {
		input_mt_slot(rmi4_data->input_dev, finger);
		input_mt_report_slot_state(rmi4_data->input_dev,
				MT_TOOL_FINGER, 0);
	}

	input_report_key(rmi4_data->input_dev, BTN_TOUCH, 0);
	input_report_key(rmi4_data->input_dev,
			BTN_TOOL_FINGER, 0);

	input_sync(rmi4_data->input_dev);
}
 //[BUGFIX]-Add-BEGIN by TCTSZ. weihong.chen, PR756816,2014/08/08, Judge if a palm present
static int synaptics_finger_count(struct synaptics_rmi4_data *rmi4_data,
		 struct synaptics_rmi4_fn *fhandler)
 {
	 int retval;
	 unsigned char touch_count = 0; /* number of touch points */
	 unsigned char reg_index;
	 unsigned char finger;
	 unsigned char fingers_supported;
	 unsigned char num_of_finger_status_regs;
	 unsigned char finger_shift;
	 unsigned char finger_status;
	 unsigned char data_reg_blk_size;
	 unsigned char finger_status_reg[3];
	 unsigned short data_addr;

	 fingers_supported = fhandler->num_of_data_points;
	 num_of_finger_status_regs = (fingers_supported + 3) / 4;
	 data_addr = fhandler->full_addr.data_base;
	 data_reg_blk_size = fhandler->size_of_data_register_block;
 
	 retval = synaptics_rmi4_i2c_read(rmi4_data,
			 data_addr,
			 finger_status_reg,
			 num_of_finger_status_regs);
	 if (retval < 0)
		 return 0;
 
	 for (finger = 0; finger < fingers_supported; finger++) {
		 reg_index = finger / 4;
		 finger_shift = (finger % 4) * 2;
		 finger_status = (finger_status_reg[reg_index] >> finger_shift)
				 & MASK_2BIT;
 
		 /*
		  * Each 2-bit finger status field represents the following:
		  * 00 = finger not present
		  * 01 = finger present and data accurate
		  * 10 = finger present but data may be inaccurate
		  * 11 = reserved
		  */

 
		 if (finger_status) {
			
			 touch_count++;
		 }
	 }
 
	
	 return touch_count;
 }
 //[BUGFIX]-Add-END by TCTSZ. weihong.chen, PR756816,2014/08/08, Judge if a palm present

 /**
 * synaptics_rmi4_f11_abs_report()
 *
 * Called by synaptics_rmi4_report_touch() when valid Function $11
 * finger data has been detected.
 *
 * This function reads the Function $11 data registers, determines the
 * status of each finger supported by the Function, processes any
 * necessary coordinate manipulation, reports the finger data to
 * the input subsystem, and returns the number of fingers detected.
 */
 
static int synaptics_rmi4_f11_abs_report(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler)
{
	int retval;
	unsigned char touch_count = 0; /* number of touch points */
	unsigned char reg_index;
	unsigned char finger;
	unsigned char fingers_supported;
	unsigned char num_of_finger_status_regs;
	unsigned char finger_shift;
	unsigned char finger_status;
	unsigned char data_reg_blk_size;
	unsigned char finger_status_reg[3];
	unsigned char data[F11_STD_DATA_LEN];
	unsigned short data_addr;
	unsigned short data_offset;
	int x;
	int y;
	int wx;
	int wy;
	int z;
	int finger_number=0;
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, add new feature
	//gesture wakeup
	#ifdef SYNAPTICS_GESTURE_WAKE_UP
	unsigned char detected_gestures;
	struct synaptics_rmi4_f11_extra_data *extra_data;
	#endif
	//palm
	#ifdef SYNAPTICS_PALM_SLEEP
	unsigned char detected_palm =0;
	#endif
  //[FEATURE]-Add-END by TCTSZ.weihong.chen
	//gw

	/*
	 * The number of finger status registers is determined by the
	 * maximum number of fingers supported - 2 bits per finger. So
	 * the number of finger status registers to read is:
	 * register_count = ceil(max_num_of_fingers / 4)
	 */
	fingers_supported = fhandler->num_of_data_points;
	num_of_finger_status_regs = (fingers_supported + 3) / 4;
	data_addr = fhandler->full_addr.data_base;
	data_reg_blk_size = fhandler->size_of_data_register_block;
 //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, detected wheather it is a palm in wake up state,
 //detected wether it is a double click in suspend state  
   #ifdef SYNAPTICS_PALM_SLEEP
     if((!rmi4_data->suspended)&& g_palm_lock_switch &&(rmi4_data->enable_palm_gesture) )
     	{
     	 detected_palm =0;
	     retval = synaptics_rmi4_i2c_read(rmi4_data,
				   0x004a,
				   &detected_palm,
				   sizeof(detected_palm));
        #ifdef TP_DEBUG
	    STP_DEBUG("detected_palm =0x%x \n",detected_palm);
        #endif
	  if(detected_palm == 0x02)
	  	   {
            finger_number = synaptics_finger_count(rmi4_data,fhandler);
			#ifdef TP_DEBUG
			STP_DEBUG("detected_palm =0x%x ,finger count =0x%x \n",detected_palm,finger_number);
            #endif
			if(finger_number >= 2) //add by weihong.chen 2014/08/08 ,only if more than 2 fingers detected light off the screen
		    	{
	  	    input_report_key(rmi4_data->input_dev, KEY_POWER, 1);
			input_sync(rmi4_data->input_dev);
			input_report_key(rmi4_data->input_dev, KEY_POWER, 0);
			input_sync(rmi4_data->input_dev);
            input_sync(rmi4_data->input_dev);
			rmi4_data->enable_palm_gesture =false;
			    
            return 0;
		    	}
	  	   }
     	}
   #endif
	
    #ifdef SYNAPTICS_GESTURE_WAKE_UP
   extra_data = (struct synaptics_rmi4_f11_extra_data *)fhandler->extra;
    if (rmi4_data->suspended && g_wakeup_gesture) {
	    retval = synaptics_rmi4_i2c_read(rmi4_data,
				data_addr + extra_data->data38_offset,
				&detected_gestures,
				sizeof(detected_gestures));
		if (retval < 0)
			return 0;
           #ifdef TP_DEBUG
	   STP_DEBUG("detected_gestures = %d\n",detected_gestures);
           #endif
           //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen, 2014/08/25, ignore doublick if there is a palm on tp
           if((detected_gestures&0x01) ==0x01) {
	      detected_palm =0;
              synaptics_rmi4_wakeup_gesture(rmi4_data,false);
              msleep(30);
              synaptics_rmi4_i2c_read(rmi4_data,
				   0x004a,
				   &detected_palm,
				   sizeof(detected_palm));
               if(detected_palm == 0x02)
                {
                 synaptics_rmi4_wakeup_gesture(rmi4_data,true);
                 #ifdef TP_DEBUG
                 STP_DEBUG("detected_double_clic and there is a palm on it ----->return\n");
                 #endif
                 return 0;
                }
               //[FEATURE]-Add-END by TCTSZ. weihong.chen,2014/08/25, ignore doublick if there is a palm on tp
			if(1 == g_wakeup_gesture)
			{
                        input_report_key(rmi4_data->input_dev, KEY_POWER, 1);
			input_sync(rmi4_data->input_dev);
			input_report_key(rmi4_data->input_dev, KEY_POWER, 0);
			input_sync(rmi4_data->input_dev);
			#ifdef TP_DEBUG
			STP_DEBUG("double click detected ,report KEY_POWER\n");
			#endif
            }
			else if(2 == g_wakeup_gesture)
			{
            input_report_key(rmi4_data->input_dev, KEY_UNLOCK, 1);
			input_sync(rmi4_data->input_dev);
			input_report_key(rmi4_data->input_dev, KEY_UNLOCK, 0);
			input_sync(rmi4_data->input_dev);
			#ifdef TP_DEBUG
			STP_DEBUG("double click detected ,report KEY_UNLOCK\n");
			#endif
			}
		 return 0;	
		 //Add-END by TCTSZ. weihong.chen 2014/06/18, add double cilick to unlock  screen
		}
 }

#endif	
	//[FEATURE]-Add-END by TCTSZ.weihong.chen

	//gw
	retval = synaptics_rmi4_i2c_read(rmi4_data,
			data_addr,
			finger_status_reg,
			num_of_finger_status_regs);
	if (retval < 0)
		return 0;

	for (finger = 0; finger < fingers_supported; finger++) {
		reg_index = finger / 4;
		finger_shift = (finger % 4) * 2;
		finger_status = (finger_status_reg[reg_index] >> finger_shift)
				& MASK_2BIT;

		/*
		 * Each 2-bit finger status field represents the following:
		 * 00 = finger not present
		 * 01 = finger present and data accurate
		 * 10 = finger present but data may be inaccurate
		 * 11 = reserved
		 */
#ifdef TYPE_B_PROTOCOL
		input_mt_slot(rmi4_data->input_dev, finger);
		input_mt_report_slot_state(rmi4_data->input_dev,
				MT_TOOL_FINGER, finger_status != 0);
#endif

		if (finger_status) {
			data_offset = data_addr +
					num_of_finger_status_regs +
					(finger * data_reg_blk_size);
			retval = synaptics_rmi4_i2c_read(rmi4_data,
					data_offset,
					data,
					data_reg_blk_size);
			if (retval < 0)
				return 0;

			x = (data[0] << 4) | (data[2] & MASK_4BIT);
			y = (data[1] << 4) | ((data[2] >> 4) & MASK_4BIT);
			wx = (data[3] & MASK_4BIT);
			wy = (data[3] >> 4) & MASK_4BIT;
			z = data[4];

			if (rmi4_data->flip_x)
				x = rmi4_data->sensor_max_x - x;
			if (rmi4_data->flip_y)
				y = rmi4_data->sensor_max_y - y;
            #ifdef TP_DEBUG
			STP_DEBUG("%s: Finger %d:\n"
					"status = 0x%02x\n"
					"x = %d\n"
					"y = %d\n"
					"wx = %d\n"
					"wy = %d\n",
					__func__, finger,
					finger_status,
					x, y, wx, wy);
            #endif
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_POSITION_X, x);
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_POSITION_Y, y);
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_PRESSURE, z);

#ifdef REPORT_2D_W
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_TOUCH_MAJOR, max(wx, wy));
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_TOUCH_MINOR, min(wx, wy));
#endif
#ifndef TYPE_B_PROTOCOL
			input_mt_sync(rmi4_data->input_dev);
#endif
			touch_count++;
		}
	}

	input_report_key(rmi4_data->input_dev, BTN_TOUCH, touch_count > 0);
	input_report_key(rmi4_data->input_dev,
			BTN_TOOL_FINGER, touch_count > 0);

#ifndef TYPE_B_PROTOCOL
	if (!touch_count)
		input_mt_sync(rmi4_data->input_dev);
#else
	input_mt_report_pointer_emulation(rmi4_data->input_dev, false);
#endif

	input_sync(rmi4_data->input_dev);

	return touch_count;
}

 /**
 * synaptics_rmi4_f12_abs_report()
 *
 * Called by synaptics_rmi4_report_touch() when valid Function $12
 * finger data has been detected.
 *
 * This function reads the Function $12 data registers, determines the
 * status of each finger supported by the Function, processes any
 * necessary coordinate manipulation, reports the finger data to
 * the input subsystem, and returns the number of fingers detected.
 */
static int synaptics_rmi4_f12_abs_report(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler)
{
	int retval;
	unsigned char touch_count = 0; /* number of touch points */
	unsigned char finger;
	unsigned char fingers_to_process;
	unsigned char finger_status;
	unsigned char size_of_2d_data;
	unsigned short data_addr;
	int x;
	int y;
	int wx;
	int wy;
	struct synaptics_rmi4_f12_extra_data *extra_data;
	struct synaptics_rmi4_f12_finger_data *data;
	struct synaptics_rmi4_f12_finger_data *finger_data;

	fingers_to_process = fhandler->num_of_data_points;
	data_addr = fhandler->full_addr.data_base;
	extra_data = (struct synaptics_rmi4_f12_extra_data *)fhandler->extra;
	size_of_2d_data = sizeof(struct synaptics_rmi4_f12_finger_data);

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			data_addr + extra_data->data1_offset,
			(unsigned char *)fhandler->data,
			fingers_to_process * size_of_2d_data);
	if (retval < 0)
		return 0;

	data = (struct synaptics_rmi4_f12_finger_data *)fhandler->data;

	for (finger = 0; finger < fingers_to_process; finger++) {
		finger_data = data + finger;
		finger_status = finger_data->object_type_and_status & MASK_2BIT;

		/*
		 * Each 2-bit finger status field represents the following:
		 * 00 = finger not present
		 * 01 = finger present and data accurate
		 * 10 = finger present but data may be inaccurate
		 * 11 = reserved
		 */
#ifdef TYPE_B_PROTOCOL
		input_mt_slot(rmi4_data->input_dev, finger);
		input_mt_report_slot_state(rmi4_data->input_dev,
				MT_TOOL_FINGER, finger_status != 0);
#endif

		if (finger_status) {
			x = (finger_data->x_msb << 8) | (finger_data->x_lsb);
			y = (finger_data->y_msb << 8) | (finger_data->y_lsb);
#ifdef REPORT_2D_W
			wx = finger_data->wx;
			wy = finger_data->wy;
#endif

			if (rmi4_data->flip_x)
				x = rmi4_data->sensor_max_x - x;
			if (rmi4_data->flip_y)
				y = rmi4_data->sensor_max_y - y;
            #ifdef TP_DEBUG
			STP_DEBUG("%s: Finger %d:\n"
					"status = 0x%02x\n"
					"x = %d\n"
					"y = %d\n"
					"wx = %d\n"
					"wy = %d\n",
					__func__, finger,
					finger_status,
					x, y, wx, wy);
            #endif
			input_report_key(rmi4_data->input_dev,
					BTN_TOUCH, 1);
			input_report_key(rmi4_data->input_dev,
					BTN_TOOL_FINGER, 1);
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_POSITION_X, x);
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_POSITION_Y, y);
#ifdef REPORT_2D_W
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_TOUCH_MAJOR, max(wx, wy));
			input_report_abs(rmi4_data->input_dev,
					ABS_MT_TOUCH_MINOR, min(wx, wy));
#endif
#ifndef TYPE_B_PROTOCOL
			input_mt_sync(rmi4_data->input_dev);
#endif
			touch_count++;
		}
	}

	input_report_key(rmi4_data->input_dev,
			BTN_TOUCH, touch_count > 0);
	input_report_key(rmi4_data->input_dev,
			BTN_TOOL_FINGER, touch_count > 0);
#ifndef TYPE_B_PROTOCOL
	if (!touch_count)
		input_mt_sync(rmi4_data->input_dev);
#endif
	input_mt_report_pointer_emulation(rmi4_data->input_dev, false);
	input_sync(rmi4_data->input_dev);

	return touch_count;
}

static void synaptics_rmi4_f1a_report(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler)
{
	int retval;
	unsigned char button;
	unsigned char index;
	unsigned char shift;
	unsigned char status;
	unsigned char *data;
	unsigned short data_addr = fhandler->full_addr.data_base;
	struct synaptics_rmi4_f1a_handle *f1a = fhandler->data;
	static unsigned char do_once = 1;
	static bool current_status[MAX_NUMBER_OF_BUTTONS];
#ifdef NO_0D_WHILE_2D
	static bool before_2d_status[MAX_NUMBER_OF_BUTTONS];
	static bool while_2d_status[MAX_NUMBER_OF_BUTTONS];
#endif

	if (do_once) {
		memset(current_status, 0, sizeof(current_status));
#ifdef NO_0D_WHILE_2D
		memset(before_2d_status, 0, sizeof(before_2d_status));
		memset(while_2d_status, 0, sizeof(while_2d_status));
#endif
		do_once = 0;
	}

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			data_addr,
			f1a->button_data_buffer,
			f1a->button_bitmask_size);
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to read button data registers\n",
				__func__);
		return;
	}

	data = f1a->button_data_buffer;

	for (button = 0; button < f1a->valid_button_count; button++) {
		index = button / 8;
		shift = button % 8;
		status = ((data[index] >> shift) & MASK_1BIT);

		if (current_status[button] == status)
			continue;
		else
			current_status[button] = status;
                #ifdef TP_DEBUG
		STP_DEBUG("%s: Button %d (code %d) ->%d\n",
				__func__, button,
				f1a->button_map[button],
				status);
		#endif
#ifdef NO_0D_WHILE_2D
		if (rmi4_data->fingers_on_2d == false) {
			if (status == 1) {
				before_2d_status[button] = 1;
			} else {
				if (while_2d_status[button] == 1) {
					while_2d_status[button] = 0;
					continue;
				} else {
					before_2d_status[button] = 0;
				}
			}
			input_report_key(rmi4_data->input_dev,
					f1a->button_map[button],
					status);
		} else {
			if (before_2d_status[button] == 1) {
				before_2d_status[button] = 0;
				input_report_key(rmi4_data->input_dev,
						f1a->button_map[button],
						status);
			} else {
				if (status == 1)
					while_2d_status[button] = 1;
				else
					while_2d_status[button] = 0;
			}
		}
#else
		input_report_key(rmi4_data->input_dev,
				f1a->button_map[button],
				status);
#endif
	}

	input_sync(rmi4_data->input_dev);

	return;
}

 /**
 * synaptics_rmi4_report_touch()
 *
 * Called by synaptics_rmi4_sensor_report().
 *
 * This function calls the appropriate finger data reporting function
 * based on the function handler it receives and returns the number of
 * fingers detected.
 */
static void synaptics_rmi4_report_touch(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler,
		unsigned char *touch_count)
{
	unsigned char touch_count_2d;
     #ifdef TP_DEBUG
	STP_DEBUG_FUNC();

	STP_DEBUG("%s: Function %02x reporting\n",
			__func__, fhandler->fn_number);
     #endif
	switch (fhandler->fn_number) {
	case SYNAPTICS_RMI4_F11:
		touch_count_2d = synaptics_rmi4_f11_abs_report(rmi4_data,
				fhandler);

		*touch_count += touch_count_2d;

		if (touch_count_2d)
			rmi4_data->fingers_on_2d = true;
		else
			rmi4_data->fingers_on_2d = false;
			break;

	case SYNAPTICS_RMI4_F12:
		touch_count_2d = synaptics_rmi4_f12_abs_report(rmi4_data,
				fhandler);

		if (touch_count_2d)
			rmi4_data->fingers_on_2d = true;
		else
			rmi4_data->fingers_on_2d = false;
		break;

	case SYNAPTICS_RMI4_F1A:
		synaptics_rmi4_f1a_report(rmi4_data, fhandler);
		break;

	default:
		break;
	}

	return;
}

 /**
 * synaptics_rmi4_sensor_report()
 *
 * Called by synaptics_rmi4_irq().
 *
 * This function determines the interrupt source(s) from the sensor
 * and calls synaptics_rmi4_report_touch() with the appropriate
 * function handler for each function with valid data inputs.
 */
static int synaptics_rmi4_sensor_report(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
	unsigned char touch_count = 0;
	unsigned char intr[MAX_INTR_REGISTERS];
    struct synaptics_rmi4_fn *fhandler;
	struct synaptics_rmi4_exp_fn *exp_fhandler;
	struct synaptics_rmi4_device_info *rmi;
    struct  synaptics_rmi4_f01_device_status status;
    unsigned char data[2];
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	rmi = &(rmi4_data->rmi4_mod_info);

	/*
	 * Get interrupt status information from F01 Data1 register to
	 * determine the source(s) that are flagging the interrupt.
	 */

   if((!rmi4_data->fw_updating)&&(!rmi4_data->device_is_reseting))//[BUGFIX]-Add-BEGIN by TCTSZ. weihong.chen,FR-782201 2014/09/04, fix f11_init error
    {
        retval = synaptics_rmi4_i2c_read(rmi4_data,
                                rmi4_data->f01_data_base_addr,
                                data,
                                 1);
                if (retval < 0) {
                        dev_err(&rmi4_data->i2c_client->dev,
                                        "%s: Failed to read interrupt status\n",
                                        __func__);
                        return retval;
                }
        status.data[0] =data[0];
        if((status.unconfigured&&!status.flash_prog)&&(!rmi4_data->fw_updating))
         {
             printk(KERN_CRIT"%s : spontaneous reset detected \n",__func__);
             synaptics_rmi4_set_configured(rmi4_data);
             msleep(70);
			 if(rmi4_data->in_wakeup_gesture_mode)
				    {
				     synaptics_rmi4_wakeup_gesture(rmi4_data,true);
				    }
			 if(!hall->tp_is_suspend)	/*Only reset tp when tp is not suspended.*/
				 if (hall->tp_set_sensitivity )
								 {
									hall->tp_set_sensitivity(gpio_get_value(hall->irq_gpio));
								 }
					       }
     }
	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_data_base_addr + 1,
			intr,
			rmi4_data->num_of_intr_regs);
	if (retval < 0)
		return retval;
    /*
	 * Traverse the function handler list and service the source(s)
	 * of the interrupt accordingly.
	 */
	mutex_lock(&rmi->support_fn_list_mutex);
	if (!list_empty(&rmi->support_fn_list)) {
		list_for_each_entry(fhandler, &rmi->support_fn_list, link) {
			if (fhandler->num_of_data_sources) {
				if (fhandler->intr_mask &
						intr[fhandler->intr_reg_num]) {
					synaptics_rmi4_report_touch(rmi4_data,
							fhandler, &touch_count);
				}
			}
		}
	}
	mutex_unlock(&rmi->support_fn_list_mutex);

	mutex_lock(&exp_fn_list_mutex);
	if (!list_empty(&exp_fn_list)) {
		list_for_each_entry(exp_fhandler, &exp_fn_list, link) {
			if (exp_fhandler->inserted &&
					(exp_fhandler->func_attn != NULL))
				exp_fhandler->func_attn(rmi4_data, intr[0]);
		}
	}
	mutex_unlock(&exp_fn_list_mutex);

	return touch_count;
}

 /**
 * synaptics_rmi4_irq()
 *
 * Called by the kernel when an interrupt occurs (when the sensor
 * asserts the attention irq).
 *
 * This function is the ISR thread and handles the acquisition
 * and the reporting of finger data when the presence of fingers
 * is detected.
 */
 //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, add new feature
 #ifdef SYNAPTICS_GESTURE_WAKE_UP

static irqreturn_t synaptics_rmi4_irq(int irq, void *data)
{
	struct synaptics_rmi4_data *rmi4_data = data;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
    synaptics_rmi4_sensor_report(rmi4_data);
    return IRQ_HANDLED;
}
 #else
 static irqreturn_t synaptics_rmi4_irq(int irq, void *data)
{
	struct synaptics_rmi4_data *rmi4_data = data;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
    synaptics_rmi4_sensor_report(rmi4_data);
    return IRQ_HANDLED;
}
 #endif
 //[FEATURE]-Add-END by TCTSZ.weihong.chen
#ifdef CONFIG_OF
static int synaptics_rmi4_get_button_map(struct device *dev, char *name,
				struct synaptics_rmi4_platform_data *rmi4_pdata,
				struct device_node *np)
{
	struct property *prop;
	int rc, i;
	u32 temp_val, num_buttons;
	u32 button_map[MAX_NUMBER_OF_BUTTONS];

	prop = of_find_property(np, "synaptics,button-map", NULL);
	if (prop) {
		num_buttons = prop->length / sizeof(temp_val);

		rmi4_pdata->capacitance_button_map = devm_kzalloc(dev,
				sizeof(*rmi4_pdata->capacitance_button_map),
				GFP_KERNEL);
		if (!rmi4_pdata->capacitance_button_map)
			return -ENOMEM;

		rmi4_pdata->capacitance_button_map->map = devm_kzalloc(dev,
			sizeof(*rmi4_pdata->capacitance_button_map->map) *
			MAX_NUMBER_OF_BUTTONS, GFP_KERNEL);
		if (!rmi4_pdata->capacitance_button_map->map)
			return -ENOMEM;

		if (num_buttons <= MAX_NUMBER_OF_BUTTONS) {
			rc = of_property_read_u32_array(np,
					"synaptics,button-map", button_map,
					num_buttons);
			if (rc) {
				dev_err(dev, "Unable to read key codes\n");
				return rc;
			}
			for (i = 0; i < num_buttons; i++)
				rmi4_pdata->capacitance_button_map->map[i] =
					button_map[i];
			rmi4_pdata->capacitance_button_map->nbuttons =
				num_buttons;
		} else {
			return -EINVAL;
		}
	}
	return 0;
}

static int synaptics_rmi4_get_dt_coords(struct device *dev, char *name,
				struct synaptics_rmi4_platform_data *pdata,
				struct device_node *node)
{
	u32 coords[RMI4_COORDS_ARR_SIZE];
	struct property *prop;
	struct device_node *np = (node == NULL) ? (dev->of_node) : (node);
	int coords_size, rc;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	coords_size = prop->length / sizeof(u32);
	if (coords_size != RMI4_COORDS_ARR_SIZE) {
		dev_err(dev, "invalid %s\n", name);
		return -EINVAL;
	}

	rc = of_property_read_u32_array(np, name, coords, coords_size);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Unable to read %s\n", name);
		return rc;
	}

	if (strcmp(name, "synaptics,panel-coords") == 0) {
		pdata->panel_minx = coords[0];
		pdata->panel_miny = coords[1];
		pdata->panel_maxx = coords[2];
		pdata->panel_maxy = coords[3];

		if (pdata->panel_maxx == 0 || pdata->panel_minx > 0)
			rc = -EINVAL;
		else if (pdata->panel_maxy == 0 || pdata->panel_miny > 0)
			rc = -EINVAL;

		if (rc) {
			dev_err(dev, "Invalid panel resolution %d\n", rc);
			return rc;
		}
	} else if (strcmp(name, "synaptics,display-coords") == 0) {
		pdata->disp_minx = coords[0];
		pdata->disp_miny = coords[1];
		pdata->disp_maxx = coords[2];
		pdata->disp_maxy = coords[3];
	} else {
		dev_err(dev, "unsupported property %s\n", name);
		return -EINVAL;
	}

	return 0;
}

static int synaptics_rmi4_parse_dt_children(struct device *dev,
		struct synaptics_rmi4_platform_data *rmi4_pdata,
		struct synaptics_rmi4_data *rmi4_data)
{
	struct synaptics_rmi4_device_info *rmi = &(rmi4_data->rmi4_mod_info);
	struct device_node *node = dev->of_node, *child;
	int rc = 0;
	struct synaptics_rmi4_fn *fhandler = NULL;

	for_each_child_of_node(node, child) {
		rc = of_property_read_u32(child, "synaptics,package-id",
				&rmi4_pdata->package_id);
		if (rc && (rc != -EINVAL)) {
			dev_err(dev, "Unable to read package_id\n");
			return rc;
		} else if (rc == -EINVAL) {
			rmi4_pdata->package_id = 0x00;
		}

		if (rmi4_pdata->package_id) {
			if (rmi4_pdata->package_id != rmi->package_id) {
				dev_err(dev,
					"%s: Synaptics package id don't match %d %d\n",
					__func__,
					rmi4_pdata->package_id,
					rmi->package_id);

				continue;
			}
		}

		rc = synaptics_rmi4_get_dt_coords(dev,
				"synaptics,display-coords",
				rmi4_pdata,
				child);
		if (rc && (rc != -EINVAL))
			return rc;

		rc = synaptics_rmi4_get_dt_coords(dev, "synaptics,panel-coords",
				rmi4_pdata, child);
		if (rc && (rc != -EINVAL))
			return rc;

		rc = synaptics_rmi4_get_button_map(dev, "synaptics,button-map",
				rmi4_pdata, child);
		if (rc < 0) {
			dev_err(dev, "Unable to read key codes\n");
			return rc;
		}

		mutex_lock(&rmi->support_fn_list_mutex);
		if (!list_empty(&rmi->support_fn_list)) {
			list_for_each_entry(fhandler,
					&rmi->support_fn_list, link) {
				if (fhandler->fn_number == SYNAPTICS_RMI4_F1A)
					break;
			}
		}
		mutex_unlock(&rmi->support_fn_list_mutex);

		if (fhandler != NULL && fhandler->fn_number ==
						SYNAPTICS_RMI4_F1A) {
			rc = synaptics_rmi4_capacitance_button_map(rmi4_data,
								fhandler);
			if (rc < 0) {
				dev_err(dev, "Fail to register F1A %d\n", rc);
				return rc;
			}
		}
		break;
	}

	return 0;
}

static int synaptics_rmi4_parse_dt(struct device *dev,
				struct synaptics_rmi4_platform_data *rmi4_pdata)
{
	struct device_node *np = dev->of_node;
	struct property *prop;
	u32 temp_val, num_buttons;
	u32 button_map[MAX_NUMBER_OF_BUTTONS];
	int rc, i;

	rmi4_pdata->i2c_pull_up = of_property_read_bool(np,
			"synaptics,i2c-pull-up");
	rmi4_pdata->power_down_enable = of_property_read_bool(np,
			"synaptics,power-down");
	rmi4_pdata->disable_gpios = of_property_read_bool(np,
			"synaptics,disable-gpios");
	rmi4_pdata->modify_reso = of_property_read_bool(np,
			"synaptics,modify-reso");
	rmi4_pdata->x_flip = of_property_read_bool(np, "synaptics,x-flip");
	rmi4_pdata->y_flip = of_property_read_bool(np, "synaptics,y-flip");
	rmi4_pdata->do_lockdown = of_property_read_bool(np,
			"synaptics,do-lockdown");

	rc = synaptics_rmi4_get_dt_coords(dev, "synaptics,display-coords",
			rmi4_pdata, NULL);
	if (rc && (rc != -EINVAL))
		return rc;

	rc = synaptics_rmi4_get_dt_coords(dev, "synaptics,panel-coords",
			rmi4_pdata, NULL);
	if (rc && (rc != -EINVAL))
		return rc;

	rmi4_pdata->reset_delay = RESET_DELAY;
	rc = of_property_read_u32(np, "synaptics,reset-delay", &temp_val);
	if (!rc)
		rmi4_pdata->reset_delay = temp_val;
	else if (rc != -EINVAL) {
		dev_err(dev, "Unable to read reset delay\n");
		return rc;
	}

	rc = of_property_read_string(np, "synaptics,fw-image-name",
		&rmi4_pdata->fw_image_name);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Unable to read fw image name\n");
		return rc;
	}

	/* reset, irq gpio info */
	rmi4_pdata->reset_gpio = of_get_named_gpio_flags(np,
			"synaptics,reset-gpio", 0, &rmi4_pdata->reset_flags);
	rmi4_pdata->irq_gpio = of_get_named_gpio_flags(np,
			"synaptics,irq-gpio", 0, &rmi4_pdata->irq_flags);

	rmi4_pdata->detect_device = of_property_read_bool(np,
					"synaptics,detect-device");

	if (rmi4_pdata->detect_device)
		return 0;

	prop = of_find_property(np, "synaptics,button-map", NULL);
	if (prop) {
		num_buttons = prop->length / sizeof(temp_val);

		rmi4_pdata->capacitance_button_map = devm_kzalloc(dev,
			sizeof(*rmi4_pdata->capacitance_button_map),
			GFP_KERNEL);
		if (!rmi4_pdata->capacitance_button_map)
			return -ENOMEM;

		rmi4_pdata->capacitance_button_map->map = devm_kzalloc(dev,
			sizeof(*rmi4_pdata->capacitance_button_map->map) *
			MAX_NUMBER_OF_BUTTONS, GFP_KERNEL);
		if (!rmi4_pdata->capacitance_button_map->map)
			return -ENOMEM;

		if (num_buttons <= MAX_NUMBER_OF_BUTTONS) {
			rc = of_property_read_u32_array(np,
				"synaptics,button-map", button_map,
				num_buttons);
			if (rc) {
				dev_err(dev, "Unable to read key codes\n");
				return rc;
			}
			for (i = 0; i < num_buttons; i++)
				rmi4_pdata->capacitance_button_map->map[i] =
					button_map[i];
			rmi4_pdata->capacitance_button_map->nbuttons =
				num_buttons;
		} else {
			return -EINVAL;
		}
	}

	return 0;
}
#else
static inline int synaptics_rmi4_parse_dt(struct device *dev,
				struct synaptics_rmi4_platform_data *rmi4_pdata)
{
	return 0;
}
#endif

 /**
 * synaptics_rmi4_irq_enable()
 *
 * Called by synaptics_rmi4_probe() and the power management functions
 * in this driver and also exported to other expansion Function modules
 * such as rmi_dev.
 *
 * This function handles the enabling and disabling of the attention
 * irq including the setting up of the ISR thread.
 */
static int synaptics_rmi4_irq_enable(struct synaptics_rmi4_data *rmi4_data,
		bool enable)
{
	int retval = 0;
	unsigned char *intr_status;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	if (enable) {
		if (rmi4_data->irq_enabled)
			return retval;

		intr_status = kzalloc(rmi4_data->num_of_intr_regs, GFP_KERNEL);
		if (!intr_status) {
			dev_err(&rmi4_data->i2c_client->dev,
					"%s: Failed to alloc memory\n",
					__func__);
			return -ENOMEM;
		}
		/* Clear interrupts first */
		retval = synaptics_rmi4_i2c_read(rmi4_data,
				rmi4_data->f01_data_base_addr + 1,
				intr_status,
				rmi4_data->num_of_intr_regs);
		kfree(intr_status);
		if (retval < 0)
			return retval;

		enable_irq(rmi4_data->irq);

		rmi4_data->irq_enabled = true;
	} else {
		if (rmi4_data->irq_enabled) {
			disable_irq(rmi4_data->irq);
			rmi4_data->irq_enabled = false;
		}
	}

	return retval;
}

 /**
 * synaptics_rmi4_f11_init()
 *
 * Called by synaptics_rmi4_query_device().
 *
 * This funtion parses information from the Function 11 registers
 * and determines the number of fingers supported, x and y data ranges,
 * offset to the associated interrupt status register, interrupt bit
 * mask, and gathers finger data acquisition capabilities from the query
 * registers.
 */
 //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, add new feature
 #ifdef SYNAPTICS_GESTURE_WAKE_UP
static int synaptics_rmi4_f11_init(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler,
		struct synaptics_rmi4_fn_desc *fd,
		unsigned int intr_count)
{
	int retval;
	unsigned char ii;
	unsigned char intr_offset;
	unsigned char abs_data_size;
	unsigned char abs_data_blk_size;
	//unsigned char query[F11_STD_QUERY_LEN];
	//unsigned char control[F11_STD_CTRL_LEN];
	unsigned char offset=0;
	int   	fingers_supported=0;
	//gesture wakeup
	struct synaptics_rmi4_f11_extra_data *extra_data;
	struct synaptics_rmi4_f11_query_0_5 query_0_5;
	struct synaptics_rmi4_f11_query_7_8 query_7_8;
	struct synaptics_rmi4_f11_query_9 query_9;
	struct synaptics_rmi4_f11_query_12 query_12;
	struct synaptics_rmi4_f11_query_27 query_27;
	struct synaptics_rmi4_f11_ctrl_6_9 control_6_9;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
    fhandler->fn_number = fd->fn_number;
	fhandler->num_of_data_sources = fd->intr_src_count;
	fhandler->extra = kmalloc(sizeof(*extra_data), GFP_KERNEL); //add error handling
	extra_data = (struct synaptics_rmi4_f11_extra_data *)fhandler->extra;
    retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.query_base,
			query_0_5.data,
			sizeof(query_0_5.data));
	if (retval < 0)
		return retval;

	/* Maximum number of fingers supported */
	if (query_0_5.num_of_fingers <= 4)
		fhandler->num_of_data_points = query_0_5.num_of_fingers + 1;
	else if (query_0_5.num_of_fingers == 5)
		fhandler->num_of_data_points = 10;

	rmi4_data->num_of_fingers = fhandler->num_of_data_points;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.ctrl_base + 6,
			control_6_9.data,
			sizeof(control_6_9.data));
	if (retval < 0)
		return retval;

	/* Maximum x and y */
	rmi4_data->sensor_max_x = control_6_9.sensor_max_x_pos_7_0 |
			(control_6_9.sensor_max_x_pos_11_8 << 8);
	rmi4_data->sensor_max_y = control_6_9.sensor_max_y_pos_7_0 |
			(control_6_9.sensor_max_y_pos_11_8 << 8);
	#ifdef TP_DEBUG
	STP_DEBUG("%s: Function %02x max x = %d max y = %d\n",
			__func__, fhandler->fn_number,
			rmi4_data->sensor_max_x,
			rmi4_data->sensor_max_y);
        #endif
	rmi4_data->max_touch_width = MAX_F11_TOUCH_WIDTH;

	fhandler->intr_reg_num = (intr_count + 7) / 8;
	if (fhandler->intr_reg_num != 0)
		fhandler->intr_reg_num -= 1;

	/* Set an enable bit for each data source */
	intr_offset = intr_count % 8;
	fhandler->intr_mask = 0;
	for (ii = intr_offset;
			ii < ((fd->intr_src_count & MASK_3BIT) +
			intr_offset);
			ii++)
		fhandler->intr_mask |= 1 << ii;

	abs_data_size = query_0_5.abs_data_size;
	abs_data_blk_size = 3 + (2 * (abs_data_size == 0 ? 1 : 0));
	fhandler->size_of_data_register_block = abs_data_blk_size;
	//gesture wakeup
	fhandler->data = NULL;

	offset = sizeof(query_0_5.data);

	/* query 6 */
	if (query_0_5.has_rel)
		offset += 1;

	/* queries 7 8 */
	if (query_0_5.has_gestures) {
		         
		retval = synaptics_rmi4_i2c_read(rmi4_data,
				fhandler->full_addr.query_base + offset,
				query_7_8.data,
				sizeof(query_7_8.data));
		if (retval < 0)
			return retval;

		offset += sizeof(query_7_8.data);
	}

	/* query 9 */
	if (query_0_5.has_query_9) {
		retval = synaptics_rmi4_i2c_read(rmi4_data,
				fhandler->full_addr.query_base + offset,
				query_9.data,
				sizeof(query_9.data));
		if (retval < 0)
			return retval;

		offset += sizeof(query_9.data);
	}

	/* query 10 */
	if (query_0_5.has_gestures && query_7_8.has_touch_shapes)
		offset += 1;

	/* query 11 */
	if (query_0_5.has_query_11)
		offset += 1;

	/* query 12 */
	if (query_0_5.has_query_12) {
		retval = synaptics_rmi4_i2c_read(rmi4_data,
				fhandler->full_addr.query_base + offset,
				query_12.data,
				sizeof(query_12.data));
		if (retval < 0)
			return retval;

		offset += sizeof(query_12.data);
	}

	/* query 13 */
	if (query_0_5.has_jitter_filter)
		offset += 1;

	/* query 14 */
	if (query_0_5.has_query_12 && query_12.has_general_information_2)
		offset += 1;

	/* queries 15 16 17 18 19 20 21 22 23 24 25 26*/
	if (query_0_5.has_query_12 && query_12.has_physical_properties)
		offset += 12;

	/* query 27 */
	if (query_0_5.has_query_27) {
		retval = synaptics_rmi4_i2c_read(rmi4_data,
				fhandler->full_addr.query_base + offset,
				query_27.data,
				sizeof(query_27.data));
		if (retval < 0)
			return retval;

		rmi4_data->f11_wakeup_gesture = query_27.has_wakeup_gesture;
	}

	if (!rmi4_data->f11_wakeup_gesture)
		return retval;

	/* data 0 */
	fingers_supported = fhandler->num_of_data_points;
	offset = (fingers_supported + 3) / 4;

	/* data 1 2 3 4 5 */
	offset += 5 * fingers_supported;

	/* data 6 7 */
	if (query_0_5.has_rel)
		offset += 2 * fingers_supported;

	/* data 8 */
	if (query_0_5.has_gestures && query_7_8.data[0])
		offset += 1;

	/* data 9 */
	if (query_0_5.has_gestures && (query_7_8.data[0] || query_7_8.data[1]))
		offset += 1;

	/* data 10 */
	if (query_0_5.has_gestures &&
			(query_7_8.has_pinch || query_7_8.has_flick))
		offset += 1;

	/* data 11 12 */
	if (query_0_5.has_gestures &&
			(query_7_8.has_flick || query_7_8.has_rotate))
		offset += 2;

	/* data 13 */
	if (query_0_5.has_gestures && query_7_8.has_touch_shapes)
		offset += (fingers_supported + 3) / 4;

	/* data 14 15 */
	if (query_0_5.has_gestures &&
			(query_7_8.has_scroll_zones ||
			query_7_8.has_multi_finger_scroll ||
			query_7_8.has_chiral_scroll))
		offset += 2;

	/* data 16 17 */
	if (query_0_5.has_gestures &&
			(query_7_8.has_scroll_zones &&
			query_7_8.individual_scroll_zones))
		offset += 2;

	/* data 18 19 20 21 22 23 24 25 26 27 */
	if (query_0_5.has_query_9 && query_9.has_contact_geometry)
		offset += 10 * fingers_supported;

	/* data 28 */
	if (query_0_5.has_bending_correction ||
			query_0_5.has_large_object_suppression)
		offset += 1;

	/* data 29 30 31 */
	if (query_0_5.has_query_9 && query_9.has_pen_hover_discrimination)
		offset += 3;

	/* data 32 */
	if (query_0_5.has_query_12 &&
			query_12.has_small_object_detection_tuning)
		offset += 1;

	/* data 33 34 */
	if (query_0_5.has_query_27 && query_27.f11_query27_b0)
		offset += 2;

	/* data 35 */
	if (query_0_5.has_query_12 && query_12.has_8bit_w)
		offset += fingers_supported;

	/* data 36 */
	if (query_0_5.has_bending_correction)
		offset += 1;

	/* data 37 */
	if (query_0_5.has_query_27 && query_27.has_data_37)
		offset += 1;

	/* data 38 */
	if (query_0_5.has_query_27 && query_27.has_wakeup_gesture)
		extra_data->data38_offset = offset;
	//gw

	return retval;
}

#else
static int synaptics_rmi4_f11_init(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler,
		struct synaptics_rmi4_fn_desc *fd,
		unsigned int intr_count)
{
	int retval;
	unsigned char ii;
	unsigned char intr_offset;
	unsigned char abs_data_size;
	unsigned char abs_data_blk_size;
	unsigned char query[F11_STD_QUERY_LEN];
	unsigned char control[F11_STD_CTRL_LEN];

	fhandler->fn_number = fd->fn_number;
	fhandler->num_of_data_sources = fd->intr_src_count;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.query_base,
			query,
			sizeof(query));
	if (retval < 0)
		return retval;

	/* Maximum number of fingers supported */
	if ((query[1] & MASK_3BIT) <= 4)
		fhandler->num_of_data_points = (query[1] & MASK_3BIT) + 1;
	else if ((query[1] & MASK_3BIT) == 5)
		fhandler->num_of_data_points = 10;

	rmi4_data->num_of_fingers = fhandler->num_of_data_points;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.ctrl_base,
			control,
			sizeof(control));
	if (retval < 0)
		return retval;

	/* Maximum x */
	rmi4_data->sensor_max_x = ((control[6] & MASK_8BIT) << 0) |
		((control[7] & MASK_4BIT) << 8);

	if (rmi4_data->board->modify_reso) {
		if (rmi4_data->board->panel_maxx) {
			if (rmi4_data->board->panel_maxx >= F11_MAX_X) {
				dev_err(&rmi4_data->i2c_client->dev,
					"F11 max_x value out of bound.");
				return -EINVAL;
			}
			if (rmi4_data->sensor_max_x !=
				rmi4_data->board->panel_maxx) {
				rmi4_data->sensor_max_x =
					rmi4_data->board->panel_maxx;
				control[6] = rmi4_data->board->panel_maxx
					& MASK_8BIT;
				control[7] = (rmi4_data->board->panel_maxx >> 8)
					& MASK_4BIT;
				retval = synaptics_rmi4_i2c_write(rmi4_data,
					fhandler->full_addr.ctrl_base,
					control,
					sizeof(control));
				if (retval < 0)
					return retval;
			}
		}
	}

	/* Maximum y */
	rmi4_data->sensor_max_y = ((control[8] & MASK_8BIT) << 0) |
		((control[9] & MASK_4BIT) << 8);

	if (rmi4_data->board->modify_reso) {
		if (rmi4_data->board->panel_maxy) {
			if (rmi4_data->board->panel_maxy >= F11_MAX_Y) {
				dev_err(&rmi4_data->i2c_client->dev,
					"F11 max_y value out of bound.");
				return -EINVAL;
			}
			if (rmi4_data->sensor_max_y !=
				rmi4_data->board->panel_maxy) {
				rmi4_data->sensor_max_y =
					rmi4_data->board->panel_maxy;
				control[8] = rmi4_data->board->panel_maxy
					& MASK_8BIT;
				control[9] = (rmi4_data->board->panel_maxy >> 8)
					& MASK_4BIT;
				retval = synaptics_rmi4_i2c_write(rmi4_data,
					fhandler->full_addr.ctrl_base,
					control,
					sizeof(control));
				if (retval < 0)
					return retval;
			}
		}
	}
    #ifdef TP_DEBUG
	STP_DEBUG("%s: Function %02x max x = %d max y = %d\n",
			__func__, fhandler->fn_number,
			rmi4_data->sensor_max_x,
			rmi4_data->sensor_max_y);
    #endif
	rmi4_data->max_touch_width = MAX_F11_TOUCH_WIDTH;

	fhandler->intr_reg_num = (intr_count + 7) / 8;
	if (fhandler->intr_reg_num != 0)
		fhandler->intr_reg_num -= 1;

	/* Set an enable bit for each data source */
	intr_offset = intr_count % 8;
	fhandler->intr_mask = 0;
	for (ii = intr_offset;
			ii < ((fd->intr_src_count & MASK_3BIT) +
			intr_offset);
			ii++)
		fhandler->intr_mask |= 1 << ii;

	abs_data_size = query[5] & MASK_2BIT;
	abs_data_blk_size = 3 + (2 * (abs_data_size == 0 ? 1 : 0));
	fhandler->size_of_data_register_block = abs_data_blk_size;

	return retval;
}

#endif
 //[FEATURE]-Add-END by TCTSZ.weihong.chen
static int synaptics_rmi4_f12_set_enables(struct synaptics_rmi4_data *rmi4_data,
		 unsigned short ctrl28)
 {
	 int retval;
	 static unsigned short ctrl_28_address;
 
	 if (ctrl28)
		 ctrl_28_address = ctrl28;
 
	 retval = synaptics_rmi4_i2c_write(rmi4_data,
			 ctrl_28_address,
			 &rmi4_data->report_enable,
			 sizeof(rmi4_data->report_enable));
	 if (retval < 0)
		 return retval;
 
	 return retval;
 }


 /**
 * synaptics_rmi4_f12_init()
 *
 * Called by synaptics_rmi4_query_device().
 *
 * This funtion parses information from the Function 12 registers and
 * determines the number of fingers supported, offset to the data1
 * register, x and y data ranges, offset to the associated interrupt
 * status register, interrupt bit mask, and allocates memory resources
 * for finger data acquisition.
 */
static int synaptics_rmi4_f12_init(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler,
		struct synaptics_rmi4_fn_desc *fd,
		unsigned int intr_count)
{
	int retval;
	unsigned char ii;
	unsigned char intr_offset;
	unsigned char size_of_2d_data;
	unsigned char size_of_query8;
	unsigned char ctrl_8_offset;
	unsigned char ctrl_23_offset;
	unsigned char ctrl_28_offset;
	unsigned char num_of_fingers;
	struct synaptics_rmi4_f12_extra_data *extra_data;
	struct synaptics_rmi4_f12_query_5 query_5;
	struct synaptics_rmi4_f12_query_8 query_8;
	struct synaptics_rmi4_f12_ctrl_8 ctrl_8;
	struct synaptics_rmi4_f12_ctrl_23 ctrl_23;

	fhandler->fn_number = fd->fn_number;
	fhandler->num_of_data_sources = fd->intr_src_count;
	fhandler->extra = kmalloc(sizeof(*extra_data), GFP_KERNEL);
	extra_data = (struct synaptics_rmi4_f12_extra_data *)fhandler->extra;
	size_of_2d_data = sizeof(struct synaptics_rmi4_f12_finger_data);

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.query_base + 5,
			query_5.data,
			sizeof(query_5.data));
	if (retval < 0)
		return retval;

	ctrl_8_offset = query_5.ctrl0_is_present +
			query_5.ctrl1_is_present +
			query_5.ctrl2_is_present +
			query_5.ctrl3_is_present +
			query_5.ctrl4_is_present +
			query_5.ctrl5_is_present +
			query_5.ctrl6_is_present +
			query_5.ctrl7_is_present;

	ctrl_23_offset = ctrl_8_offset +
			query_5.ctrl8_is_present +
			query_5.ctrl9_is_present +
			query_5.ctrl10_is_present +
			query_5.ctrl11_is_present +
			query_5.ctrl12_is_present +
			query_5.ctrl13_is_present +
			query_5.ctrl14_is_present +
			query_5.ctrl15_is_present +
			query_5.ctrl16_is_present +
			query_5.ctrl17_is_present +
			query_5.ctrl18_is_present +
			query_5.ctrl19_is_present +
			query_5.ctrl20_is_present +
			query_5.ctrl21_is_present +
			query_5.ctrl22_is_present;

	ctrl_28_offset = ctrl_23_offset +
			query_5.ctrl23_is_present +
			query_5.ctrl24_is_present +
			query_5.ctrl25_is_present +
			query_5.ctrl26_is_present +
			query_5.ctrl27_is_present;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.ctrl_base + ctrl_23_offset,
			ctrl_23.data,
			sizeof(ctrl_23.data));
	if (retval < 0)
		return retval;

	/* Maximum number of fingers supported */
	fhandler->num_of_data_points = min(ctrl_23.max_reported_objects,
			(unsigned char)F12_FINGERS_TO_SUPPORT);

	num_of_fingers = fhandler->num_of_data_points;
	rmi4_data->num_of_fingers = num_of_fingers;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.query_base + 7,
			&size_of_query8,
			sizeof(size_of_query8));
	if (retval < 0)
		return retval;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.query_base + 8,
			query_8.data,
			size_of_query8);
	if (retval < 0)
		return retval;

	/* Determine the presence of the Data0 register */
	extra_data->data1_offset = query_8.data0_is_present;

	if ((size_of_query8 >= 3) && (query_8.data15_is_present)) {
		extra_data->data15_offset = query_8.data0_is_present +
				query_8.data1_is_present +
				query_8.data2_is_present +
				query_8.data3_is_present +
				query_8.data4_is_present +
				query_8.data5_is_present +
				query_8.data6_is_present +
				query_8.data7_is_present +
				query_8.data8_is_present +
				query_8.data9_is_present +
				query_8.data10_is_present +
				query_8.data11_is_present +
				query_8.data12_is_present +
				query_8.data13_is_present +
				query_8.data14_is_present;
		extra_data->data15_size = (num_of_fingers + 7) / 8;
	} else {
		extra_data->data15_size = 0;
	}

	rmi4_data->report_enable = RPT_DEFAULT;
#ifdef REPORT_2D_Z
	rmi4_data->report_enable |= RPT_Z;
#endif
#ifdef REPORT_2D_W
	rmi4_data->report_enable |= (RPT_WX | RPT_WY);
#endif

	retval = synaptics_rmi4_f12_set_enables(rmi4_data,
			fhandler->full_addr.ctrl_base + ctrl_28_offset);
	if (retval < 0)
		return retval;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.ctrl_base + ctrl_8_offset,
			ctrl_8.data,
			sizeof(ctrl_8.data));
	if (retval < 0)
		return retval;

	/* Maximum x */
	rmi4_data->sensor_max_x =
		((unsigned short)ctrl_8.max_x_coord_lsb << 0) |
		((unsigned short)ctrl_8.max_x_coord_msb << 8);

	if (rmi4_data->board->modify_reso) {
		if (rmi4_data->board->panel_maxx) {
			if (rmi4_data->board->panel_maxx >= F12_MAX_X) {
				dev_err(&rmi4_data->i2c_client->dev,
					"F12 max_x value out of bound.");
				return -EINVAL;
			}
			if (rmi4_data->sensor_max_x !=
					rmi4_data->board->panel_maxx) {
				rmi4_data->sensor_max_x =
					rmi4_data->board->panel_maxx;
				ctrl_8.max_x_coord_lsb = (unsigned char)
					(rmi4_data->board->panel_maxx
					& MASK_8BIT);
				ctrl_8.max_x_coord_msb = (unsigned char)
					((rmi4_data->board->panel_maxx >> 8)
					& MASK_8BIT);
				retval = synaptics_rmi4_i2c_write(rmi4_data,
					fhandler->full_addr.ctrl_base
						+ ctrl_8_offset,
					ctrl_8.data,
					sizeof(ctrl_8.data));
				if (retval < 0)
					return retval;
			}
		}
	}

	/* Maximum y */
	rmi4_data->sensor_max_y =
		((unsigned short)ctrl_8.max_y_coord_lsb << 0) |
		((unsigned short)ctrl_8.max_y_coord_msb << 8);

	if (rmi4_data->board->modify_reso) {
		if (rmi4_data->board->panel_maxy) {
			if (rmi4_data->board->panel_maxy >= F12_MAX_Y) {
				dev_err(&rmi4_data->i2c_client->dev,
					"F12 max_y value out of bound.");
					return -EINVAL;
			}
			if (rmi4_data->sensor_max_y !=
				rmi4_data->board->panel_maxy) {
				rmi4_data->sensor_max_y =
					rmi4_data->board->panel_maxy;
				ctrl_8.max_y_coord_lsb = (unsigned char)
					(rmi4_data->board->panel_maxy
					& MASK_8BIT);
				ctrl_8.max_y_coord_msb = (unsigned char)
					((rmi4_data->board->panel_maxy >> 8)
					& MASK_8BIT);
				retval = synaptics_rmi4_i2c_write(rmi4_data,
					fhandler->full_addr.ctrl_base
						+ ctrl_8_offset,
					ctrl_8.data,
					sizeof(ctrl_8.data));
				if (retval < 0)
					return retval;
			}
		}
	}
        #ifdef TP_DEBUG
	STP_DEBUG("%s: Function %02x max x = %d max y = %d\n",
			__func__, fhandler->fn_number,
			rmi4_data->sensor_max_x,
			rmi4_data->sensor_max_y);
	#endif

	rmi4_data->num_of_rx = ctrl_8.num_of_rx;
	rmi4_data->num_of_tx = ctrl_8.num_of_tx;
	rmi4_data->max_touch_width = max(rmi4_data->num_of_rx,
			rmi4_data->num_of_tx);

	fhandler->intr_reg_num = (intr_count + 7) / 8;
	if (fhandler->intr_reg_num != 0)
		fhandler->intr_reg_num -= 1;

	/* Set an enable bit for each data source */
	intr_offset = intr_count % 8;
	fhandler->intr_mask = 0;
	for (ii = intr_offset;
			ii < ((fd->intr_src_count & MASK_3BIT) +
			intr_offset);
			ii++)
		fhandler->intr_mask |= 1 << ii;

	/* Allocate memory for finger data storage space */
	fhandler->data_size = num_of_fingers * size_of_2d_data;
	fhandler->data = kmalloc(fhandler->data_size, GFP_KERNEL);

	return retval;
}

static int synaptics_rmi4_f1a_alloc_mem(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler)
{
	int retval;
	struct synaptics_rmi4_f1a_handle *f1a;

	f1a = kzalloc(sizeof(*f1a), GFP_KERNEL);
	if (!f1a) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to alloc mem for function handle\n",
				__func__);
		return -ENOMEM;
	}

	fhandler->data = (void *)f1a;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			fhandler->full_addr.query_base,
			f1a->button_query.data,
			sizeof(f1a->button_query.data));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to read query registers\n",
				__func__);
		return retval;
	}

	f1a->button_count = f1a->button_query.max_button_count + 1;
	f1a->button_bitmask_size = (f1a->button_count + 7) / 8;

	f1a->button_data_buffer = kcalloc(f1a->button_bitmask_size,
			sizeof(*(f1a->button_data_buffer)), GFP_KERNEL);
	if (!f1a->button_data_buffer) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to alloc mem for data buffer\n",
				__func__);
		return -ENOMEM;
	}

	f1a->button_map = kcalloc(f1a->button_count,
			sizeof(*(f1a->button_map)), GFP_KERNEL);
	if (!f1a->button_map) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to alloc mem for button map\n",
				__func__);
		return -ENOMEM;
	}

	return 0;
}

static int synaptics_rmi4_capacitance_button_map(
				struct synaptics_rmi4_data *rmi4_data,
				struct synaptics_rmi4_fn *fhandler)
{
	unsigned char ii;
	struct synaptics_rmi4_f1a_handle *f1a = fhandler->data;
	const struct synaptics_rmi4_platform_data *pdata = rmi4_data->board;

	if (!pdata->capacitance_button_map) {
		dev_info(&rmi4_data->i2c_client->dev,
				"%s: capacitance_button_map not in use\n",
				__func__);
		return 0;
	} else if (!pdata->capacitance_button_map->map) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Button map is missing in board file\n",
				__func__);
		return -ENODEV;
	} else {
		if (pdata->capacitance_button_map->nbuttons !=
			f1a->button_count) {
			f1a->valid_button_count = min(f1a->button_count,
				pdata->capacitance_button_map->nbuttons);
		} else {
			f1a->valid_button_count = f1a->button_count;
		}

		for (ii = 0; ii < f1a->valid_button_count; ii++)
			f1a->button_map[ii] =
					pdata->capacitance_button_map->map[ii];
	}

	return 0;
}

static void synaptics_rmi4_f1a_kfree(struct synaptics_rmi4_fn *fhandler)
{
	struct synaptics_rmi4_f1a_handle *f1a = fhandler->data;

	if (f1a) {
		kfree(f1a->button_data_buffer);
		kfree(f1a->button_map);
		kfree(f1a);
		fhandler->data = NULL;
	}

	return;
}

static int synaptics_rmi4_f1a_init(struct synaptics_rmi4_data *rmi4_data,
		struct synaptics_rmi4_fn *fhandler,
		struct synaptics_rmi4_fn_desc *fd,
		unsigned int intr_count)
{
	int retval;
	unsigned char ii;
	unsigned short intr_offset;

	fhandler->fn_number = fd->fn_number;
	fhandler->num_of_data_sources = fd->intr_src_count;

	fhandler->intr_reg_num = (intr_count + 7) / 8;
	if (fhandler->intr_reg_num != 0)
		fhandler->intr_reg_num -= 1;

	/* Set an enable bit for each data source */
	intr_offset = intr_count % 8;
	fhandler->intr_mask = 0;
	for (ii = intr_offset;
			ii < ((fd->intr_src_count & MASK_3BIT) +
			intr_offset);
			ii++)
		fhandler->intr_mask |= 1 << ii;

	retval = synaptics_rmi4_f1a_alloc_mem(rmi4_data, fhandler);
	if (retval < 0)
		goto error_exit;

	retval = synaptics_rmi4_capacitance_button_map(rmi4_data, fhandler);
	if (retval < 0)
		goto error_exit;

	rmi4_data->button_0d_enabled = 1;

	return 0;

error_exit:
	synaptics_rmi4_f1a_kfree(fhandler);

	return retval;
}

static int synaptics_rmi4_alloc_fh(struct synaptics_rmi4_fn **fhandler,
		struct synaptics_rmi4_fn_desc *rmi_fd, int page_number)
{
	*fhandler = kzalloc(sizeof(**fhandler), GFP_KERNEL);
	if (!(*fhandler))
		return -ENOMEM;

	(*fhandler)->full_addr.data_base =
			(rmi_fd->data_base_addr |
			(page_number << 8));
	(*fhandler)->full_addr.ctrl_base =
			(rmi_fd->ctrl_base_addr |
			(page_number << 8));
	(*fhandler)->full_addr.cmd_base =
			(rmi_fd->cmd_base_addr |
			(page_number << 8));
	(*fhandler)->full_addr.query_base =
			(rmi_fd->query_base_addr |
			(page_number << 8));
	(*fhandler)->fn_number = rmi_fd->fn_number;

	return 0;
}


 /**
 * synaptics_rmi4_query_device_info()
 *
 * Called by synaptics_rmi4_query_device().
 *
 */
static int synaptics_rmi4_query_device_info(
					struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
	unsigned char f01_query[F01_STD_QUERY_LEN];
	struct synaptics_rmi4_device_info *rmi = &(rmi4_data->rmi4_mod_info);
	unsigned char pkg_id[4];

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_query_base_addr,
			f01_query,
			sizeof(f01_query));
	if (retval < 0)
		return retval;

	/* RMI Version 4.0 currently supported */
	rmi->version_major = 4;
	rmi->version_minor = 0;

	rmi->manufacturer_id = f01_query[0];
	rmi->product_props = f01_query[1];
	rmi->product_info[0] = f01_query[2] & MASK_7BIT;
	rmi->product_info[1] = f01_query[3] & MASK_7BIT;
	rmi->date_code[0] = f01_query[4] & MASK_5BIT;
	rmi->date_code[1] = f01_query[5] & MASK_4BIT;
	rmi->date_code[2] = f01_query[6] & MASK_5BIT;
	rmi->tester_id = ((f01_query[7] & MASK_7BIT) << 8) |
			(f01_query[8] & MASK_7BIT);
	rmi->serial_number = ((f01_query[9] & MASK_7BIT) << 8) |
			(f01_query[10] & MASK_7BIT);
	memcpy(rmi->product_id_string, &f01_query[11], 10);

	if (rmi->manufacturer_id != 1) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Non-Synaptics device found, manufacturer ID = %d\n",
				__func__, rmi->manufacturer_id);
	}

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_query_base_addr + F01_PACKAGE_ID_OFFSET,
			pkg_id,
			sizeof(pkg_id));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to read device package id (code %d)\n",
				__func__, retval);
		return retval;
	}

	rmi->package_id = (pkg_id[1] << 8) | pkg_id[0];
	rmi->package_id_rev = (pkg_id[3] << 8) | pkg_id[2];

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_query_base_addr + F01_BUID_ID_OFFSET,
			rmi->build_id,
			sizeof(rmi->build_id));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to read firmware build id (code %d)\n",
				__func__, retval);
		return retval;
	}
	return 0;
}

/*
* This function checks whether the fhandler already existis in the
* support_fn_list or not.
* If it exists then return 1 as found or return 0 as not found.
*
* Called by synaptics_rmi4_query_device().
*/
static int synaptics_rmi4_check_fn_list(struct synaptics_rmi4_data *rmi4_data,
				struct synaptics_rmi4_fn *fhandler)
{
	int found = 0;
	struct synaptics_rmi4_fn *new_fhandler;
	struct synaptics_rmi4_device_info *rmi;

	rmi = &(rmi4_data->rmi4_mod_info);

	mutex_lock(&rmi->support_fn_list_mutex);
	if (!list_empty(&rmi->support_fn_list))
		list_for_each_entry(new_fhandler, &rmi->support_fn_list, link)
			if (new_fhandler->fn_number == fhandler->fn_number)
				found = 1;
	mutex_unlock(&rmi->support_fn_list_mutex);

	return found;
}

 /**
 * synaptics_rmi4_query_device()
 *
 * Called by synaptics_rmi4_probe().
 *
 * This funtion scans the page description table, records the offsets
 * to the register types of Function $01, sets up the function handlers
 * for Function $11 and Function $12, determines the number of interrupt
 * sources from the sensor, adds valid Functions with data inputs to the
 * Function linked list, parses information from the query registers of
 * Function $01, and enables the interrupt sources from the valid Functions
 * with data inputs.
 */
static int synaptics_rmi4_query_device(struct synaptics_rmi4_data *rmi4_data)
{
	int retval, found;
	unsigned char ii;
	unsigned char page_number;
	unsigned char intr_count = 0;
	unsigned char data_sources = 0;
	unsigned short pdt_entry_addr;
	unsigned short intr_addr;
	struct synaptics_rmi4_f01_device_status status;
	struct synaptics_rmi4_fn_desc rmi_fd;
	struct synaptics_rmi4_fn *fhandler;
	struct synaptics_rmi4_device_info *rmi;

	rmi = &(rmi4_data->rmi4_mod_info);

	/* Scan the page description tables of the pages to service */
	for (page_number = 0; page_number < PAGES_TO_SERVICE; page_number++) {
		for (pdt_entry_addr = PDT_START; pdt_entry_addr > PDT_END;
				pdt_entry_addr -= PDT_ENTRY_SIZE) {
			pdt_entry_addr |= (page_number << 8);

			retval = synaptics_rmi4_i2c_read(rmi4_data,
					pdt_entry_addr,
					(unsigned char *)&rmi_fd,
					sizeof(rmi_fd));
			if (retval < 0)
				return retval;

			fhandler = NULL;
			found = 0;
			if (rmi_fd.fn_number == 0) {
				#ifdef TP_DEBUG
				STP_DEBUG("%s: Reached end of PDT\n",
						__func__);
				#endif
				break;
			}
            #ifdef TP_DEBUG
			STP_DEBUG("%s: F%02x found (page %d)\n",
					__func__, rmi_fd.fn_number,
					page_number);
            #endif
			switch (rmi_fd.fn_number) {
			case SYNAPTICS_RMI4_F01:
				rmi4_data->f01_query_base_addr =
						rmi_fd.query_base_addr;
				rmi4_data->f01_ctrl_base_addr =
						rmi_fd.ctrl_base_addr;
				rmi4_data->f01_data_base_addr =
						rmi_fd.data_base_addr;
				rmi4_data->f01_cmd_base_addr =
						rmi_fd.cmd_base_addr;

				retval =
				synaptics_rmi4_query_device_info(rmi4_data);
				if (retval < 0)
					return retval;

				retval = synaptics_rmi4_i2c_read(rmi4_data,
						rmi4_data->f01_data_base_addr,
						status.data,
						sizeof(status.data));
				if (retval < 0)
					return retval;

				while (status.status_code == STATUS_CRC_IN_PROGRESS) {
					msleep(1);
					retval = synaptics_rmi4_i2c_read(rmi4_data,
						rmi4_data->f01_data_base_addr,
						status.data,
						sizeof(status.data));
					if (retval < 0)
						return retval;
				}

				if (status.flash_prog == 1) {
					pr_notice("%s: In flash prog mode, status = 0x%02x\n",
							__func__,
							status.status_code);
					goto flash_prog_mode;
				}
				break;

			case SYNAPTICS_RMI4_F11:
				if (rmi_fd.intr_src_count == 0)
					break;

				retval = synaptics_rmi4_alloc_fh(&fhandler,
						&rmi_fd, page_number);
				if (retval < 0) {
					dev_err(&rmi4_data->i2c_client->dev,
							"%s: Failed to alloc for F%d\n",
							__func__,
							rmi_fd.fn_number);
					return retval;
				}

				retval = synaptics_rmi4_f11_init(rmi4_data,
						fhandler, &rmi_fd, intr_count);
				if (retval < 0)
					return retval;
				break;

			case SYNAPTICS_RMI4_F12:
				if (rmi_fd.intr_src_count == 0)
					break;

				retval = synaptics_rmi4_alloc_fh(&fhandler,
						&rmi_fd, page_number);
				if (retval < 0) {
					dev_err(&rmi4_data->i2c_client->dev,
							"%s: Failed to alloc for F%d\n",
							__func__,
							rmi_fd.fn_number);
					return retval;
				}

				retval = synaptics_rmi4_f12_init(rmi4_data,
						fhandler, &rmi_fd, intr_count);
				if (retval < 0)
					return retval;
				break;

			case SYNAPTICS_RMI4_F1A:
				if (rmi_fd.intr_src_count == 0)
					break;

				retval = synaptics_rmi4_alloc_fh(&fhandler,
						&rmi_fd, page_number);
				if (retval < 0) {
					dev_err(&rmi4_data->i2c_client->dev,
							"%s: Failed to alloc for F%d\n",
							__func__,
							rmi_fd.fn_number);
					return retval;
				}

				retval = synaptics_rmi4_f1a_init(rmi4_data,
						fhandler, &rmi_fd, intr_count);
				if (retval < 0)
					return retval;
				break;
			}

			/* Accumulate the interrupt count */
			intr_count += (rmi_fd.intr_src_count & MASK_3BIT);

			if (fhandler && rmi_fd.intr_src_count) {
				/* Want to check whether the fhandler already
				exists in the support_fn_list or not.
				If not found then add it to the list, otherwise
				free the memory allocated to it.
				*/
				found = synaptics_rmi4_check_fn_list(rmi4_data,
						fhandler);

				if (!found) {
					mutex_lock(&rmi->support_fn_list_mutex);
					list_add_tail(&fhandler->link,
							&rmi->support_fn_list);
					mutex_unlock(
						&rmi->support_fn_list_mutex);
				} else {
					if (fhandler->fn_number ==
							SYNAPTICS_RMI4_F1A) {
						synaptics_rmi4_f1a_kfree(
							fhandler);
					} else {
						kfree(fhandler->data);
						kfree(fhandler->extra);
					}
					kfree(fhandler);
				}
			}
		}
	}

flash_prog_mode:
	rmi4_data->num_of_intr_regs = (intr_count + 7) / 8;
    #ifdef TP_DEBUG
	STP_DEBUG("%s: Number of interrupt registers = %d\n",
			__func__, rmi4_data->num_of_intr_regs);
    #endif
	memset(rmi4_data->intr_mask, 0x00, sizeof(rmi4_data->intr_mask));

	/*
	 * Map out the interrupt bit masks for the interrupt sources
	 * from the registered function handlers.
	 */
	mutex_lock(&rmi->support_fn_list_mutex);
	if (!list_empty(&rmi->support_fn_list)) {
		list_for_each_entry(fhandler, &rmi->support_fn_list, link)
			data_sources += fhandler->num_of_data_sources;
	}
	mutex_unlock(&rmi->support_fn_list_mutex);

	if (data_sources) {
		mutex_lock(&rmi->support_fn_list_mutex);
		if (!list_empty(&rmi->support_fn_list)) {
			list_for_each_entry(fhandler,
						&rmi->support_fn_list, link) {
				if (fhandler->num_of_data_sources) {
					rmi4_data->intr_mask[fhandler->intr_reg_num] |=
						fhandler->intr_mask;
				}
			}
		}
		mutex_unlock(&rmi->support_fn_list_mutex);
	}

	/* Enable the interrupt sources */
	for (ii = 0; ii < rmi4_data->num_of_intr_regs; ii++) {
		if (rmi4_data->intr_mask[ii] != 0x00) {
			#ifdef TP_DEBUG
			STP_DEBUG("%s: Interrupt enable mask %d = 0x%02x\n",
					__func__, ii, rmi4_data->intr_mask[ii]);
			#endif
			intr_addr = rmi4_data->f01_ctrl_base_addr + 1 + ii;
			retval = synaptics_rmi4_i2c_write(rmi4_data,
					intr_addr,
					&(rmi4_data->intr_mask[ii]),
					sizeof(rmi4_data->intr_mask[ii]));
			if (retval < 0)
				return retval;
		}
	}

	return 0;
}

static int synaptics_rmi4_reset_command(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
	int page_number;
	unsigned char command = 0x01;
	unsigned short pdt_entry_addr;
	struct synaptics_rmi4_fn_desc rmi_fd;
	bool done = false;

	/* Scan the page description tables of the pages to service */
	for (page_number = 0; page_number < PAGES_TO_SERVICE; page_number++) {
		for (pdt_entry_addr = PDT_START; pdt_entry_addr > PDT_END;
				pdt_entry_addr -= PDT_ENTRY_SIZE) {
			retval = synaptics_rmi4_i2c_read(rmi4_data,
				pdt_entry_addr,
				(unsigned char *)&rmi_fd,
				sizeof(rmi_fd));
			if (retval < 0)
				return retval;

			if (rmi_fd.fn_number == 0)
				break;

			switch (rmi_fd.fn_number) {
			case SYNAPTICS_RMI4_F01:
				rmi4_data->f01_cmd_base_addr =
					rmi_fd.cmd_base_addr;
				done = true;
				break;
			}
		}
		if (done) {
			dev_info(&rmi4_data->i2c_client->dev,
				"%s: Find F01 in page description table 0x%x\n",
				__func__, rmi4_data->f01_cmd_base_addr);
			break;
		}
	}

	if (!done) {
		dev_err(&rmi4_data->i2c_client->dev,
			"%s: Cannot find F01 in page description table\n",
			__func__);
		return -EINVAL;
	}

	retval = synaptics_rmi4_i2c_write(rmi4_data,
			rmi4_data->f01_cmd_base_addr,
			&command,
			sizeof(command));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to issue reset command, error = %d\n",
				__func__, retval);
		return retval;
	}

	msleep(rmi4_data->board->reset_delay);
	return retval;
};

static int synaptics_rmi4_reset_device(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
	struct synaptics_rmi4_fn *fhandler;
	struct synaptics_rmi4_fn *next_fhandler;
	struct synaptics_rmi4_device_info *rmi;

	rmi = &(rmi4_data->rmi4_mod_info);

	retval = synaptics_rmi4_reset_command(rmi4_data);
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
			"%s: Failed to send command reset\n",
			__func__);
		return retval;
	}

	if (!list_empty(&rmi->support_fn_list)) {
		list_for_each_entry_safe(fhandler, next_fhandler,
					&rmi->support_fn_list, link) {
			if (fhandler->fn_number == SYNAPTICS_RMI4_F1A)
				synaptics_rmi4_f1a_kfree(fhandler);
			else {
				kfree(fhandler->data);
				kfree(fhandler->extra);
			}
			kfree(fhandler);
		}
	}

	INIT_LIST_HEAD(&rmi->support_fn_list);

	retval = synaptics_rmi4_query_device(rmi4_data);
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to query device\n",
				__func__);
		return retval;
	}

	return 0;
}

/**
* synaptics_rmi4_detection_work()
*
* Called by the kernel at the scheduled time.
*
* This function is a self-rearming work thread that checks for the
* insertion and removal of other expansion Function modules such as
* rmi_dev and calls their initialization and removal callback functions
* accordingly.
*/
static void synaptics_rmi4_detection_work(struct work_struct *work)
{
	struct synaptics_rmi4_exp_fn *exp_fhandler, *next_list_entry;
	struct synaptics_rmi4_data *rmi4_data =
			container_of(work, struct synaptics_rmi4_data,
			det_work.work);

	mutex_lock(&exp_fn_list_mutex);
	if (!list_empty(&exp_fn_list)) {
		list_for_each_entry_safe(exp_fhandler,
				next_list_entry,
				&exp_fn_list,
				link) {
			if ((exp_fhandler->func_init != NULL) &&
					(exp_fhandler->inserted == false)) {
				if (exp_fhandler->func_init(rmi4_data) < 0) {
					list_del(&exp_fhandler->link);
					kfree(exp_fhandler);
				} else {
					exp_fhandler->inserted = true;
				}
			} else if ((exp_fhandler->func_init == NULL) &&
					(exp_fhandler->inserted == true)) {
				exp_fhandler->func_remove(rmi4_data);
				list_del(&exp_fhandler->link);
				kfree(exp_fhandler);
			}
		}
	}
	mutex_unlock(&exp_fn_list_mutex);

	return;
}

/**
* synaptics_rmi4_new_function()
*
* Called by other expansion Function modules in their module init and
* module exit functions.
*
* This function is used by other expansion Function modules such as
* rmi_dev to register themselves with the driver by providing their
* initialization and removal callback function pointers so that they
* can be inserted or removed dynamically at module init and exit times,
* respectively.
*/
void synaptics_rmi4_new_function(enum exp_fn fn_type, bool insert,
		int (*func_init)(struct synaptics_rmi4_data *rmi4_data),
		void (*func_remove)(struct synaptics_rmi4_data *rmi4_data),
		void (*func_attn)(struct synaptics_rmi4_data *rmi4_data,
		unsigned char intr_mask))
{
	struct synaptics_rmi4_exp_fn *exp_fhandler;
    if (!exp_fn_inited) {
		mutex_init(&exp_fn_list_mutex);
		INIT_LIST_HEAD(&exp_fn_list);
		exp_fn_inited = 1;
	}

	mutex_lock(&exp_fn_list_mutex);
	if (insert) {
		exp_fhandler = kzalloc(sizeof(*exp_fhandler), GFP_KERNEL);
		if (!exp_fhandler) {
			pr_err("%s: Failed to alloc mem for expansion function\n",
					__func__);
			goto exit;
		}
		exp_fhandler->fn_type = fn_type;
		exp_fhandler->func_init = func_init;
		exp_fhandler->func_attn = func_attn;
		exp_fhandler->func_remove = func_remove;
		exp_fhandler->inserted = false;
		list_add_tail(&exp_fhandler->link, &exp_fn_list);
	} else {
		if (!list_empty(&exp_fn_list)) {
			list_for_each_entry(exp_fhandler, &exp_fn_list, link) {
				if (exp_fhandler->func_init == func_init) {
					exp_fhandler->inserted = false;
					exp_fhandler->func_init = NULL;
					exp_fhandler->func_attn = NULL;
					goto exit;
				}
			}
		}
	}

exit:
	mutex_unlock(&exp_fn_list_mutex);

	return;
}
EXPORT_SYMBOL(synaptics_rmi4_new_function);


static int reg_set_optimum_mode_check(struct regulator *reg, int load_uA)
{
	return (regulator_count_voltages(reg) > 0) ?
		regulator_set_optimum_mode(reg, load_uA) : 0;
}

static int synaptics_rmi4_regulator_configure(struct synaptics_rmi4_data
						*rmi4_data, bool on)
{
	int retval;

	if (on == false)
		goto hw_shutdown;

	rmi4_data->vdd = regulator_get(&rmi4_data->i2c_client->dev,
					"vdd");
	if (IS_ERR(rmi4_data->vdd)) {
		dev_err(&rmi4_data->i2c_client->dev,
				"%s: Failed to get vdd regulator\n",
				__func__);
		return PTR_ERR(rmi4_data->vdd);
	}

	if (regulator_count_voltages(rmi4_data->vdd) > 0) {
		retval = regulator_set_voltage(rmi4_data->vdd,
			RMI4_VTG_MIN_UV, RMI4_VTG_MAX_UV);
		if (retval) {
			dev_err(&rmi4_data->i2c_client->dev,
				"regulator set_vtg failed retval =%d\n",
				retval);
			goto err_set_vtg_vdd;
		}
	}

	if (rmi4_data->board->i2c_pull_up) {
		rmi4_data->vcc_i2c = regulator_get(&rmi4_data->i2c_client->dev,
						"vcc_i2c");
		if (IS_ERR(rmi4_data->vcc_i2c)) {
			dev_err(&rmi4_data->i2c_client->dev,
					"%s: Failed to get i2c regulator\n",
					__func__);
			retval = PTR_ERR(rmi4_data->vcc_i2c);
			goto err_get_vtg_i2c;
		}

		if (regulator_count_voltages(rmi4_data->vcc_i2c) > 0) {
			retval = regulator_set_voltage(rmi4_data->vcc_i2c,
				RMI4_I2C_VTG_MIN_UV, RMI4_I2C_VTG_MAX_UV);
			if (retval) {
				dev_err(&rmi4_data->i2c_client->dev,
					"reg set i2c vtg failed retval =%d\n",
					retval);
			goto err_set_vtg_i2c;
			}
		}
	}
	return 0;

err_set_vtg_i2c:
	if (rmi4_data->board->i2c_pull_up)
		regulator_put(rmi4_data->vcc_i2c);
err_get_vtg_i2c:
	if (regulator_count_voltages(rmi4_data->vdd) > 0)
		regulator_set_voltage(rmi4_data->vdd, 0,
			RMI4_VTG_MAX_UV);
err_set_vtg_vdd:
	regulator_put(rmi4_data->vdd);
	return retval;

hw_shutdown:
	if (regulator_count_voltages(rmi4_data->vdd) > 0)
		regulator_set_voltage(rmi4_data->vdd, 0,
			RMI4_VTG_MAX_UV);
	regulator_put(rmi4_data->vdd);
	if (rmi4_data->board->i2c_pull_up) {
		if (regulator_count_voltages(rmi4_data->vcc_i2c) > 0)
			regulator_set_voltage(rmi4_data->vcc_i2c, 0,
					RMI4_I2C_VTG_MAX_UV);
		regulator_put(rmi4_data->vcc_i2c);
	}
	return 0;
};

static int synaptics_rmi4_power_on(struct synaptics_rmi4_data *rmi4_data,
					bool on) {
	int retval;

	if (on == false)
		goto power_off;

	retval = reg_set_optimum_mode_check(rmi4_data->vdd,
		RMI4_ACTIVE_LOAD_UA);
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
			"Regulator vdd set_opt failed rc=%d\n",
			retval);
		return retval;
	}

	retval = regulator_enable(rmi4_data->vdd);
	if (retval) {
		dev_err(&rmi4_data->i2c_client->dev,
			"Regulator vdd enable failed rc=%d\n",
			retval);
		goto error_reg_en_vdd;
	}

	if (rmi4_data->board->i2c_pull_up) {
		retval = reg_set_optimum_mode_check(rmi4_data->vcc_i2c,
			RMI4_I2C_LOAD_UA);
		if (retval < 0) {
			dev_err(&rmi4_data->i2c_client->dev,
				"Regulator vcc_i2c set_opt failed rc=%d\n",
				retval);
			goto error_reg_opt_i2c;
		}

		retval = regulator_enable(rmi4_data->vcc_i2c);
		if (retval) {
			dev_err(&rmi4_data->i2c_client->dev,
				"Regulator vcc_i2c enable failed rc=%d\n",
				retval);
			goto error_reg_en_vcc_i2c;
		}
	}
	return 0;

error_reg_en_vcc_i2c:
	if (rmi4_data->board->i2c_pull_up)
		reg_set_optimum_mode_check(rmi4_data->vcc_i2c, 0);
error_reg_opt_i2c:
	regulator_disable(rmi4_data->vdd);
error_reg_en_vdd:
	reg_set_optimum_mode_check(rmi4_data->vdd, 0);
	return retval;

power_off:
	reg_set_optimum_mode_check(rmi4_data->vdd, 0);
	regulator_disable(rmi4_data->vdd);
	if (rmi4_data->board->i2c_pull_up) {
		reg_set_optimum_mode_check(rmi4_data->vcc_i2c, 0);
		regulator_disable(rmi4_data->vcc_i2c);
	}
	return 0;
}

static int synaptics_rmi4_pinctrl_init(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;

	/* Get pinctrl if target uses pinctrl */
	rmi4_data->ts_pinctrl = devm_pinctrl_get(&(rmi4_data->i2c_client->dev));
	if (IS_ERR_OR_NULL(rmi4_data->ts_pinctrl)) {
		#ifdef TP_DEBUG
		STP_DEBUG("Target does not use pinctrl\n");
		#endif
		retval = PTR_ERR(rmi4_data->ts_pinctrl);
		dev_dbg(&rmi4_data->i2c_client->dev,
			"Target does not use pinctrl %d\n", retval);
		goto err_pinctrl_get;
	}

	rmi4_data->pinctrl_state_active
		#ifdef  CONFIG_TCT_8X16_POP8LTE
		//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, fix the compile bug,some where already use "pmx_ts_active"
		= pinctrl_lookup_state(rmi4_data->ts_pinctrl, "synaptics_ts_active");
		//[FEATURE]-Add-END by TCTSZ.weihong.chen
		#else
		= pinctrl_lookup_state(rmi4_data->ts_pinctrl,"pmx_ts_active");
		#endif

	if (IS_ERR_OR_NULL(rmi4_data->pinctrl_state_active)) {
		#ifdef TP_DEBUG
		STP_DEBUG("Can not get ts default pinstate\n");
		#endif
		retval = PTR_ERR(rmi4_data->pinctrl_state_active);
		rmi4_data->ts_pinctrl = NULL;
		return retval;
	}

	rmi4_data->pinctrl_state_suspend
		#ifdef  CONFIG_TCT_8X16_POP8LTE
		//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, fix the compile bug,some where already use "pmx_ts_suspend"
		= pinctrl_lookup_state(rmi4_data->ts_pinctrl, "synaptics_ts_suspend");
		//[FEATURE]-Add-END by TCTSZ.weihong.chen
		#else
		= pinctrl_lookup_state(rmi4_data->ts_pinctrl, "pmx_ts_suspend");
		#endif
	if (IS_ERR_OR_NULL(rmi4_data->pinctrl_state_suspend)) {
		#ifdef TP_DEBUG
		STP_DEBUG("Can not get ts sleep pinstate\n");
		#endif
		retval = PTR_ERR(rmi4_data->pinctrl_state_suspend);
		rmi4_data->ts_pinctrl = NULL;
		return retval;
	}

	return 0;
    err_pinctrl_get:
	rmi4_data->ts_pinctrl = NULL;
	return retval;
}

static int synpatics_rmi4_pinctrl_select(struct synaptics_rmi4_data *rmi4_data,
						bool on)
{
	struct pinctrl_state *pins_state;
	int ret;
    int retval=0;
	pins_state = on ? rmi4_data->pinctrl_state_active
		: rmi4_data->pinctrl_state_suspend;
	if (!IS_ERR_OR_NULL(pins_state)) {
		ret = pinctrl_select_state(rmi4_data->ts_pinctrl, pins_state);
		if (ret) {
			#ifdef  CONFIG_TCT_8X16_POP8LTE
			dev_err(&rmi4_data->i2c_client->dev,
				"can not set %s pins\n",
				on ? "synaptics_ts_active" : "synaptics_ts_suspend");
			#else
             dev_err(&rmi4_data->i2c_client->dev,
				"can not set %s pins\n",
				on ? "pmx_ts_active" : "pmx_ts_suspend");
			#endif

			
			return ret;
		}
	} else
	    #ifdef  CONFIG_TCT_8X16_POP8LTE
		dev_err(&rmi4_data->i2c_client->dev,
			"not a valid '%s' pinstate\n",
				on ? "synaptics_ts_active" : "synaptics_ts_suspend");
		#else
		dev_err(&rmi4_data->i2c_client->dev,
			"not a valid '%s' pinstate\n",
				on ? "pmx_ts_active" : "pmx_ts_suspend");
		#endif
	rmi4_data->pinctrl_state_active
		= pinctrl_lookup_state(rmi4_data->ts_pinctrl,
				PINCTRL_STATE_ACTIVE);
	if (IS_ERR_OR_NULL(rmi4_data->pinctrl_state_active)) {
		retval = PTR_ERR(rmi4_data->pinctrl_state_active);
		dev_err(&rmi4_data->i2c_client->dev,
			"Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_ACTIVE, retval);
		goto err_pinctrl_lookup;
	}

	rmi4_data->pinctrl_state_suspend
		= pinctrl_lookup_state(rmi4_data->ts_pinctrl,
				PINCTRL_STATE_SUSPEND);
	if (IS_ERR_OR_NULL(rmi4_data->pinctrl_state_suspend)) {
		retval = PTR_ERR(rmi4_data->pinctrl_state_suspend);
		dev_err(&rmi4_data->i2c_client->dev,
			"Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_SUSPEND, retval);
		goto err_pinctrl_lookup;
	}

	rmi4_data->pinctrl_state_release
		= pinctrl_lookup_state(rmi4_data->ts_pinctrl,
			PINCTRL_STATE_RELEASE);
	if (IS_ERR_OR_NULL(rmi4_data->pinctrl_state_release)) {
		retval = PTR_ERR(rmi4_data->pinctrl_state_release);
		dev_dbg(&rmi4_data->i2c_client->dev,
			"Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_RELEASE, retval);
	}
	return 0;
err_pinctrl_lookup:
	devm_pinctrl_put(rmi4_data->ts_pinctrl);
//err_pinctrl_get:
	rmi4_data->ts_pinctrl = NULL;
	return retval;
}

static int synaptics_rmi4_gpio_configure(struct synaptics_rmi4_data *rmi4_data,
					bool on)
{
	int retval = 0;

	if (on) {
		if (gpio_is_valid(rmi4_data->board->irq_gpio)) {
			/* configure touchscreen irq gpio */
			retval = gpio_request(rmi4_data->board->irq_gpio,
				"rmi4_irq_gpio");
			if (retval) {
				dev_err(&rmi4_data->i2c_client->dev,
					"unable to request gpio [%d]\n",
					rmi4_data->board->irq_gpio);
				goto err_irq_gpio_req;
			}
			retval = gpio_direction_input(rmi4_data->board->\
				irq_gpio);
			if (retval) {
				dev_err(&rmi4_data->i2c_client->dev,
					"unable to set direction for gpio " \
					"[%d]\n", rmi4_data->board->irq_gpio);
				goto err_irq_gpio_dir;
			}
		} else {
			dev_err(&rmi4_data->i2c_client->dev,
				"irq gpio not provided\n");
			goto err_irq_gpio_req;
		}

		if (gpio_is_valid(rmi4_data->board->reset_gpio)) {
			/* configure touchscreen reset out gpio */
			retval = gpio_request(rmi4_data->board->reset_gpio,
					"rmi4_reset_gpio");
			if (retval) {
				dev_err(&rmi4_data->i2c_client->dev,
					"unable to request gpio [%d]\n",
					rmi4_data->board->reset_gpio);
				goto err_irq_gpio_dir;
			}

			retval = gpio_direction_output(rmi4_data->board->\
				reset_gpio, 1);
			if (retval) {
				dev_err(&rmi4_data->i2c_client->dev,
					"unable to set direction for gpio " \
					"[%d]\n", rmi4_data->board->reset_gpio);
				goto err_reset_gpio_dir;
			}

			gpio_set_value(rmi4_data->board->reset_gpio, 1);
			//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, reset the IC first
			#ifdef CONFIG_TCT_8X16_POP8LTE
			msleep(1);
			gpio_set_value(rmi4_data->board->reset_gpio, 0);
			msleep(1);
			gpio_set_value(rmi4_data->board->reset_gpio, 1);
			#endif
			//[FEATURE]-Add-END by TCTSZ.weihong.chen
			msleep(rmi4_data->board->reset_delay);
		} else
			synaptics_rmi4_reset_command(rmi4_data);

		return 0;
	} else {
		if (rmi4_data->board->disable_gpios) {
			//[MOD]-Add-BEGIN by TCTSZ. weihong.chen,2014/07/21, reduce leakage current
			if (gpio_is_valid(rmi4_data->board->irq_gpio))
				{
				    #ifdef CONFIG_TCT_8X16_POP8LTE
                    retval = gpio_direction_input(rmi4_data->
							board->irq_gpio);
				    if (retval) {
					dev_err(&rmi4_data->i2c_client->dev,
					"unable to set direction for irq "
					"[%d]\n", rmi4_data->board->irq_gpio);
				    }
					#endif
			        gpio_free(rmi4_data->board->irq_gpio);
				  }
			//[MOD]-Add-BEGIN by TCTSZ. weihong.chen,2014/07/21, reduce leakage current
			if (gpio_is_valid(rmi4_data->board->reset_gpio)) {
				/*
				 * This is intended to save leakage current
				 * only. Even if the call(gpio_direction_input)
				 * fails, only leakage current will be more but
				 * functionality will not be affected.
				 */
				retval = gpio_direction_input(rmi4_data->
							board->reset_gpio);
				if (retval) {
					dev_err(&rmi4_data->i2c_client->dev,
					"unable to set direction for gpio "
					"[%d]\n", rmi4_data->board->irq_gpio);
				}
				gpio_free(rmi4_data->board->reset_gpio);
			}
		}

		return 0;
	}

err_reset_gpio_dir:
	if (gpio_is_valid(rmi4_data->board->reset_gpio))
		gpio_free(rmi4_data->board->reset_gpio);
err_irq_gpio_dir:
	if (gpio_is_valid(rmi4_data->board->irq_gpio))
		gpio_free(rmi4_data->board->irq_gpio);
err_irq_gpio_req:
	return retval;
}

 /**
 * synaptics_rmi4_probe()
 *
 * Called by the kernel when an association with an I2C device of the
 * same name is made (after doing i2c_add_driver).
 *
 * This funtion allocates and initializes the resources for the driver
 * as an input driver, turns on the power to the sensor, queries the
 * sensor for its supported Functions and characteristics, registers
 * the driver to the input subsystem, sets up the interrupt, handles
 * the registration of the early_suspend and late_resume functions,
 * and creates a work queue for detection of other expansion Function
 * modules.
 */
 
static int synaptics_rmi4_probe(struct i2c_client *client,
		const struct i2c_device_id *dev_id)
{
	int retval = 0;
	unsigned char ii;
	unsigned char attr_count;
	struct synaptics_rmi4_f1a_handle *f1a;
	struct synaptics_rmi4_fn *fhandler;
	struct synaptics_rmi4_fn *next_fhandler;
	struct synaptics_rmi4_data *rmi4_data;
	struct synaptics_rmi4_device_info *rmi;
	struct synaptics_rmi4_platform_data *platform_data =
			client->dev.platform_data;
	struct dentry *temp;
        #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
	#endif

	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev,
				"%s: SMBus byte data not supported\n",
				__func__);
		return -EIO;
	}

	if (client->dev.of_node) {
		platform_data = devm_kzalloc(&client->dev,
			sizeof(*platform_data),
			GFP_KERNEL);
		if (!platform_data) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		retval = synaptics_rmi4_parse_dt(&client->dev, platform_data);
		if (retval)
			return retval;
	} else {
		platform_data = client->dev.platform_data;
	}

	if (!platform_data) {
		dev_err(&client->dev,
				"%s: No platform data found\n",
				__func__);
		return -EINVAL;
	}

	rmi4_data = kzalloc(sizeof(*rmi4_data) * 2, GFP_KERNEL);
	if (!rmi4_data) {
		dev_err(&client->dev,
				"%s: Failed to alloc mem for rmi4_data\n",
				__func__);
		return -ENOMEM;
	}
	
	rmi = &(rmi4_data->rmi4_mod_info);

	rmi4_data->input_dev = input_allocate_device();
	if (rmi4_data->input_dev == NULL) {
		dev_err(&client->dev,
				"%s: Failed to allocate input device\n",
				__func__);
		retval = -ENOMEM;
		goto err_input_device;
	}

	rmi4_data->i2c_client = client;
	rmi4_data->current_page = MASK_8BIT;
	rmi4_data->board = platform_data;
	rmi4_data->touch_stopped = false;
	rmi4_data->sensor_sleep = false;
	rmi4_data->irq_enabled = false;
	rmi4_data->fw_updating = false;
	rmi4_data->suspended = false;
	#ifdef CONFIG_TCT_8X16_POP8LTE
	rmi4_data->enable_palm_gesture =true;
	rmi4_data->in_wakeup_gesture_mode = 0;
    rmi4_data->device_is_reseting =false;//[BUGFIX]-Add-BEGIN by TCTSZ. weihong.chen,FR-782201 2014/09/04, fix f11_init error
	#endif
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, add new feature
	/*#ifdef SYNAPTICS_GESTURE_WAKE_UP
	rmi4_data->enable_wakeup_gesture = true;  
    #endif
	#ifdef SYNAPTICS_PALM_SLEEP
	rmi4_data->enable_palm_gesture = true;
     #endif
     	*/
	//[FEATURE]-Add-END by TCTSZ.weihong.chen
	rmi4_data->i2c_read = synaptics_rmi4_i2c_read;
	rmi4_data->i2c_write = synaptics_rmi4_i2c_write;
	rmi4_data->irq_enable = synaptics_rmi4_irq_enable;
	rmi4_data->reset_device = synaptics_rmi4_reset_device;

	rmi4_data->flip_x = rmi4_data->board->x_flip;
	rmi4_data->flip_y = rmi4_data->board->y_flip;

	if (rmi4_data->board->fw_image_name)
		snprintf(rmi4_data->fw_image_name, NAME_BUFFER_SIZE, "%s",
			rmi4_data->board->fw_image_name);

	rmi4_data->input_dev->name = DRIVER_NAME;
	rmi4_data->input_dev->phys = INPUT_PHYS_NAME;
	rmi4_data->input_dev->id.bustype = BUS_I2C;
	rmi4_data->input_dev->id.product = SYNAPTICS_DSX_DRIVER_PRODUCT;
	rmi4_data->input_dev->id.version = SYNAPTICS_DSX_DRIVER_VERSION;
	rmi4_data->input_dev->dev.parent = &client->dev;
	input_set_drvdata(rmi4_data->input_dev, rmi4_data);

	set_bit(EV_SYN, rmi4_data->input_dev->evbit);
	set_bit(EV_KEY, rmi4_data->input_dev->evbit);
	set_bit(EV_ABS, rmi4_data->input_dev->evbit);
	set_bit(BTN_TOUCH, rmi4_data->input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, rmi4_data->input_dev->keybit);
#ifdef INPUT_PROP_DIRECT
	set_bit(INPUT_PROP_DIRECT, rmi4_data->input_dev->propbit);
#endif
   
	retval = synaptics_rmi4_regulator_configure(rmi4_data, true);
	if (retval < 0) {
		dev_err(&client->dev, "Failed to configure regulators\n");
		goto err_reg_configure;
	}

	retval = synaptics_rmi4_power_on(rmi4_data, true);
	if (retval < 0) {
		dev_err(&client->dev, "Failed to power on\n");
		goto err_power_device;
	}

	retval = synaptics_rmi4_pinctrl_init(rmi4_data);
	if (!retval && rmi4_data->ts_pinctrl) {
		retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_active);
		if (retval < 0)
			goto err_pinctrl_select;
	} else {
		goto err_pinctrl_init;
	}

	retval = synaptics_rmi4_gpio_configure(rmi4_data, true);
	if (retval < 0) {
		dev_err(&client->dev, "Failed to configure gpios\n");
		goto err_gpio_config;
	}

	init_waitqueue_head(&rmi4_data->wait);
	mutex_init(&(rmi4_data->rmi4_io_ctrl_mutex));

	INIT_LIST_HEAD(&rmi->support_fn_list);
	mutex_init(&rmi->support_fn_list_mutex);

	retval = synaptics_rmi4_query_device(rmi4_data);
	if (retval < 0) {
		dev_err(&client->dev,
				"%s: Failed to query device\n",
				__func__);
		goto err_free_gpios;
	}
   #ifdef CONFIG_TCT_8X16_POP8LTE
   synaptics_rmi4_set_configured(rmi4_data);
   #endif
   //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen, 2014/06/24, change  sensitive  when the hall is closed
   #ifdef  HAVE_HALL
   g_rmi4_data  = rmi4_data;
   hall->tp_set_sensitivity =tp_set_sensitivity;
   hall->tp_set_sensitivity(gpio_get_value(hall->irq_gpio));	
   hall->tp_is_suspend =0;  //add by weihong.chen 2014/07/07 ,to indicate weather call  tp_set_sensitivity()
   #endif
   //[FEATURE]-END-BEGIN by TCTSZ. weihong.chen, 2014/06/24, change  sensitive  when the hall is closed
	if (platform_data->detect_device) {
		retval = synaptics_rmi4_parse_dt_children(&client->dev,
				platform_data, rmi4_data);
		if (retval < 0)
			dev_err(&client->dev,
				"%s: Failed to parse device tree property\n",
					__func__);
	}

	if (rmi4_data->board->disp_maxx)
		rmi4_data->disp_maxx = rmi4_data->board->disp_maxx;
	else
		rmi4_data->disp_maxx = rmi4_data->sensor_max_x;

	if (rmi4_data->board->disp_maxy)
		rmi4_data->disp_maxy = rmi4_data->board->disp_maxy;
	else
		rmi4_data->disp_maxy = rmi4_data->sensor_max_y;

	if (rmi4_data->board->disp_minx)
		rmi4_data->disp_minx = rmi4_data->board->disp_minx;
	else
		rmi4_data->disp_minx = 0;

	if (rmi4_data->board->disp_miny)
		rmi4_data->disp_miny = rmi4_data->board->disp_miny;
	else
		rmi4_data->disp_miny = 0;

	input_set_abs_params(rmi4_data->input_dev,
			ABS_MT_POSITION_X, rmi4_data->disp_minx,
			rmi4_data->disp_maxx, 0, 0);
	input_set_abs_params(rmi4_data->input_dev,
			ABS_MT_POSITION_Y, rmi4_data->disp_miny,
			rmi4_data->disp_maxy, 0, 0);
	input_set_abs_params(rmi4_data->input_dev,
			ABS_PRESSURE, 0, 255, 0, 0);
	
 
#ifdef REPORT_2D_W
	input_set_abs_params(rmi4_data->input_dev,
			ABS_MT_TOUCH_MAJOR, 0,
			rmi4_data->max_touch_width, 0, 0);
	input_set_abs_params(rmi4_data->input_dev,
			ABS_MT_TOUCH_MINOR, 0,
			rmi4_data->max_touch_width, 0, 0);
#endif
      
#ifdef TYPE_B_PROTOCOL
	input_mt_init_slots(rmi4_data->input_dev,
			rmi4_data->num_of_fingers, 0);
#endif
//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, add new feature
#ifdef SYNAPTICS_GESTURE_WAKE_UP
    input_set_capability(rmi4_data->input_dev, EV_KEY, KEY_POWER);
    input_set_capability(rmi4_data->input_dev, EV_KEY, KEY_UNLOCK); //Add by TCTSZ. weihong.chen 2014/06/18, add double cilick to unlock 
#endif
//[FEATURE]-Add-END by TCTSZ.weihong.chen
   i2c_set_clientdata(client, rmi4_data);

	f1a = NULL;
	mutex_lock(&rmi->support_fn_list_mutex);
	if (!list_empty(&rmi->support_fn_list)) {
		list_for_each_entry(fhandler, &rmi->support_fn_list, link) {
			if (fhandler->fn_number == SYNAPTICS_RMI4_F1A)
				f1a = fhandler->data;
		}
	}
	mutex_unlock(&rmi->support_fn_list_mutex);

	if (f1a) {
		for (ii = 0; ii < f1a->valid_button_count; ii++) {
			set_bit(f1a->button_map[ii],
					rmi4_data->input_dev->keybit);
			input_set_capability(rmi4_data->input_dev,
					EV_KEY, f1a->button_map[ii]);
		}
	}
   retval = input_register_device(rmi4_data->input_dev);
	if (retval) {
		dev_err(&client->dev,
				"%s: Failed to register input device\n",
				__func__);
		goto err_register_input;
	}

	configure_sleep(rmi4_data);

	if (!exp_fn_inited) {
		mutex_init(&exp_fn_list_mutex);
		INIT_LIST_HEAD(&exp_fn_list);
		exp_fn_inited = 1;
	}

    #if GTP_ESD_PROTECT
    rmi4_data->clk_tick_cnt = 2 * HZ;      // HZ: clock ticks in 1 second generated by system
    printk(KERN_CRIT"Clock ticks for an esd cycle: %d", rmi4_data->clk_tick_cnt);
    spin_lock_init(&rmi4_data->esd_lock);
    // ts->esd_lock = SPIN_LOCK_UNLOCKED;
    
    INIT_DELAYED_WORK(&rmi4_data->gtp_esd_check_work, gtp_esd_check_func);
    gtp_esd_check_workqueue = create_workqueue("gtp_esd_check");
    #endif
    
	rmi4_data->det_workqueue =
			create_singlethread_workqueue("rmi_det_workqueue");
	INIT_DELAYED_WORK(&rmi4_data->det_work,
			synaptics_rmi4_detection_work);
	queue_delayed_work(rmi4_data->det_workqueue,
			&rmi4_data->det_work,
			msecs_to_jiffies(EXP_FN_DET_INTERVAL));

	rmi4_data->irq = gpio_to_irq(platform_data->irq_gpio);
	
   //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, trigger our work on hardware interrupt
   
	#ifdef SYNAPTICS_GESTURE_WAKE_UP
 //   spin_lock_init(rmi4_data->irq_lock);    
	
	retval = request_threaded_irq(rmi4_data->irq,NULL,
	synaptics_rmi4_irq, platform_data->irq_flags,
		DRIVER_NAME, rmi4_data);
	//[FEATURE]-Add-END by TCTSZ.weihong.chen
	#else
    retval = request_threaded_irq(rmi4_data->irq, NULL,
		synaptics_rmi4_irq, platform_data->irq_flags,
		DRIVER_NAME, rmi4_data);
	#endif
   rmi4_data->irq_enabled = true;
    
	if (retval < 0) {
		dev_err(&client->dev,
				"%s: Failed to create irq thread\n",
				__func__);
		goto err_enable_irq;
	}

	rmi4_data->dir = debugfs_create_dir(DEBUGFS_DIR_NAME, NULL);
	if (rmi4_data->dir == NULL || IS_ERR(rmi4_data->dir)) {
		dev_err(&client->dev,
			"%s: Failed to create debugfs directory, rc = %ld\n",
			__func__, PTR_ERR(rmi4_data->dir));
		retval = PTR_ERR(rmi4_data->dir);
		goto err_create_debugfs_dir;
	}

	temp = debugfs_create_file("suspend", S_IRUSR | S_IWUSR, rmi4_data->dir,
					rmi4_data, &debug_suspend_fops);
	if (temp == NULL || IS_ERR(temp)) {
		dev_err(&client->dev,
			"%s: Failed to create suspend debugfs file, rc = %ld\n",
			__func__, PTR_ERR(temp));
		retval = PTR_ERR(temp);
		goto err_create_debugfs_file;
	}

	for (attr_count = 0; attr_count < ARRAY_SIZE(attrs); attr_count++) {
		retval = sysfs_create_file(&client->dev.kobj,
				&attrs[attr_count].attr);
		if (retval < 0) {
			dev_err(&client->dev,
					"%s: Failed to create sysfs attributes\n",
					__func__);
			goto err_sysfs;
		}
	}

	synaptics_rmi4_sensor_wake(rmi4_data);

	retval = synaptics_rmi4_irq_enable(rmi4_data, true);
	if (retval < 0) {
		dev_err(&client->dev,
			"%s: Failed to enable attention interrupt\n",
			__func__);
		goto err_sysfs;
	}

	retval = synaptics_rmi4_check_configuration(rmi4_data);
	if (retval < 0) {
		dev_err(&client->dev, "Failed to check configuration\n");
		return retval;
	}
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, get firmware version for /proc/tct_debug/tp/firmware_version
         #ifdef  SYNAPTICS_VERSION
	 g_tp_firmware_ver = synaptics_read_firmware_version(rmi4_data);
	 g_tp_cfg_ver =synaptics_read_cfg_version(rmi4_data);//add by weihong.chen 2014/06/16  add show cfg version
	 g_tp_device_name = "S7020";
	 #endif
   //[FEATURE]-Add-END by TCTSZ. weihong.chen
        
	return retval;

err_sysfs:
	for (attr_count--; attr_count >= 0; attr_count--) {
		sysfs_remove_file(&rmi4_data->input_dev->dev.kobj,
				&attrs[attr_count].attr);
	}
err_create_debugfs_file:
	debugfs_remove_recursive(rmi4_data->dir);
err_create_debugfs_dir:
	free_irq(rmi4_data->irq, rmi4_data);
err_enable_irq:
	cancel_delayed_work_sync(&rmi4_data->det_work);
	flush_workqueue(rmi4_data->det_workqueue);
	destroy_workqueue(rmi4_data->det_workqueue);
	
	input_unregister_device(rmi4_data->input_dev);

err_register_input:
	mutex_lock(&rmi->support_fn_list_mutex);
	if (!list_empty(&rmi->support_fn_list)) {
		list_for_each_entry_safe(fhandler, next_fhandler,
					&rmi->support_fn_list, link) {
			if (fhandler->fn_number == SYNAPTICS_RMI4_F1A)
				synaptics_rmi4_f1a_kfree(fhandler);
			else {
				kfree(fhandler->data);
				kfree(fhandler->extra);
			}
			kfree(fhandler);
		}
	}
	mutex_unlock(&rmi->support_fn_list_mutex);
err_free_gpios:
	if (gpio_is_valid(rmi4_data->board->reset_gpio))
		gpio_free(rmi4_data->board->reset_gpio);
	if (gpio_is_valid(rmi4_data->board->irq_gpio))
		gpio_free(rmi4_data->board->irq_gpio);
//pinctrl_sleep:
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, use pinctrl_put(rmi4_data->ts_pinctrl) to release    pinctrl
	/*
	if (rmi4_data->ts_pinctrl) {		
		retval = synpatics_rmi4_pinctrl_select(rmi4_data, false);	
		if (retval < 0)			
			pr_err("Cannot get idle pinctrl state\n");	
		}
       */
    if (rmi4_data->ts_pinctrl) {	
		    synpatics_rmi4_pinctrl_select(rmi4_data, false);
			//#ifdef CONFIG_TCT_8X16_POP8LTE
	    	//pinctrl_put(rmi4_data->ts_pinctrl);
			//#endif
    	}
	 //[FEATURE]-Add-END by TCTSZ. weihong.chen,PR-674715 2014/05/26,
	
err_gpio_config:
err_pinctrl_select:
	if (rmi4_data->ts_pinctrl) {
		if (IS_ERR_OR_NULL(rmi4_data->pinctrl_state_release)) {
			devm_pinctrl_put(rmi4_data->ts_pinctrl);
			rmi4_data->ts_pinctrl = NULL;
		} else {
			retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_release);
			if (retval)
				pr_err("failed to select release pinctrl state\n");
		}
	}
err_pinctrl_init:
	synaptics_rmi4_power_on(rmi4_data, false);
err_power_device:
	synaptics_rmi4_regulator_configure(rmi4_data, false);
err_reg_configure:
	input_free_device(rmi4_data->input_dev);
	rmi4_data->input_dev = NULL;
err_input_device:
	kfree(rmi4_data);
  //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, return a negative value to indicate probe is failed 
   #ifdef CONFIG_TCT_8X16_POP8LTE
   return -EIO;
   #else
   return retval;
   #endif
   //[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen

}

 /**
 * synaptics_rmi4_remove()
 *
 * Called by the kernel when the association with an I2C device of the
 * same name is broken (when the driver is unloaded).
 *
 * This funtion terminates the work queue, stops sensor data acquisition,
 * frees the interrupt, unregisters the driver from the input subsystem,
 * turns off the power to the sensor, and frees other allocated resources.
 */
static int synaptics_rmi4_remove(struct i2c_client *client)
{
	unsigned char attr_count;
	struct synaptics_rmi4_fn *fhandler;
	struct synaptics_rmi4_fn *next_fhandler;
	struct synaptics_rmi4_data *rmi4_data = i2c_get_clientdata(client);
	struct synaptics_rmi4_device_info *rmi;
	int retval;

	rmi = &(rmi4_data->rmi4_mod_info);

	debugfs_remove_recursive(rmi4_data->dir);
	cancel_delayed_work_sync(&rmi4_data->det_work);
	flush_workqueue(rmi4_data->det_workqueue);
	destroy_workqueue(rmi4_data->det_workqueue);

	rmi4_data->touch_stopped = true;
	wake_up(&rmi4_data->wait);

	free_irq(rmi4_data->irq, rmi4_data);

	for (attr_count = 0; attr_count < ARRAY_SIZE(attrs); attr_count++) {
		sysfs_remove_file(&rmi4_data->input_dev->dev.kobj,
				&attrs[attr_count].attr);
	}

	input_unregister_device(rmi4_data->input_dev);

	mutex_lock(&rmi->support_fn_list_mutex);
	if (!list_empty(&rmi->support_fn_list)) {
		list_for_each_entry_safe(fhandler, next_fhandler,
					&rmi->support_fn_list, link) {
			if (fhandler->fn_number == SYNAPTICS_RMI4_F1A)
				synaptics_rmi4_f1a_kfree(fhandler);
			else {
				kfree(fhandler->data);
				kfree(fhandler->extra);
			}
			kfree(fhandler);
		}
	}
	mutex_unlock(&rmi->support_fn_list_mutex);

	if (gpio_is_valid(rmi4_data->board->reset_gpio))
		gpio_free(rmi4_data->board->reset_gpio);
	if (gpio_is_valid(rmi4_data->board->irq_gpio))
		gpio_free(rmi4_data->board->irq_gpio);

	if (rmi4_data->ts_pinctrl) {
		if (IS_ERR_OR_NULL(rmi4_data->pinctrl_state_release)) {
			devm_pinctrl_put(rmi4_data->ts_pinctrl);
			rmi4_data->ts_pinctrl = NULL;
		} else {
			retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_release);
			if (retval < 0)
				pr_err("failed to select release pinctrl state\n");
		}
	}

	synaptics_rmi4_power_on(rmi4_data, false);
	synaptics_rmi4_regulator_configure(rmi4_data, false);

	kfree(rmi4_data);

	return 0;
}

 /**
 * synaptics_rmi4_sensor_sleep()
 *
 * Called by synaptics_rmi4_early_suspend() and synaptics_rmi4_suspend().
 *
 * This function stops finger data acquisition and puts the sensor to sleep.
 */
static void synaptics_rmi4_sensor_sleep(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
	struct synaptics_rmi4_f01_device_control_0 device_ctrl;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			device_ctrl.data,
			sizeof(device_ctrl.data));
	if (retval < 0) {
		dev_err(&(rmi4_data->input_dev->dev),
				"%s: Failed to enter sleep mode\n",
				__func__);
		rmi4_data->sensor_sleep = false;
		return;
	}

	device_ctrl.sleep_mode = SENSOR_SLEEP;
	device_ctrl.nosleep = NO_SLEEP_OFF;

	retval = synaptics_rmi4_i2c_write(rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			device_ctrl.data,
			sizeof(device_ctrl.data));
	if (retval < 0) {
		dev_err(&(rmi4_data->input_dev->dev),
				"%s: Failed to enter sleep mode\n",
				__func__);
		rmi4_data->sensor_sleep = false;
		return;
	} else {
		rmi4_data->sensor_sleep = true;
	}

	return;
}

 /**
 * synaptics_rmi4_sensor_wake()
 *
 * Called by synaptics_rmi4_resume() and synaptics_rmi4_late_resume().
 *
 * This function wakes the sensor from sleep.
 */
static void synaptics_rmi4_sensor_wake(struct synaptics_rmi4_data *rmi4_data)
{
	int retval;
	struct synaptics_rmi4_f01_device_control_0 device_ctrl;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			device_ctrl.data,
			sizeof(device_ctrl.data));
	if (retval < 0) {
		dev_err(&(rmi4_data->input_dev->dev),
				"%s: Failed to wake from sleep mode\n",
				__func__);
		rmi4_data->sensor_sleep = true;
		return;
	}

	if (device_ctrl.nosleep == NO_SLEEP_OFF &&
		device_ctrl.sleep_mode == NORMAL_OPERATION) {
		rmi4_data->sensor_sleep = false;
		return;
	}

	device_ctrl.sleep_mode = NORMAL_OPERATION;
	device_ctrl.nosleep = NO_SLEEP_OFF;

	retval = synaptics_rmi4_i2c_write(rmi4_data,
			rmi4_data->f01_ctrl_base_addr,
			device_ctrl.data,
			sizeof(device_ctrl.data));
	if (retval < 0) {
		dev_err(&(rmi4_data->input_dev->dev),
				"%s: Failed to wake from sleep mode\n",
				__func__);
		rmi4_data->sensor_sleep = true;
		return;
	} else {
		rmi4_data->sensor_sleep = false;
	}

	return;
}

#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct synaptics_rmi4_data *rmi4_data =
		container_of(self, struct synaptics_rmi4_data, fb_notif);
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	if (evdata && evdata->data && event == FB_EVENT_BLANK &&
		rmi4_data && rmi4_data->i2c_client) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK)
			synaptics_rmi4_resume(&(rmi4_data->input_dev->dev));
		else if (*blank == FB_BLANK_POWERDOWN)
			synaptics_rmi4_suspend(&(rmi4_data->input_dev->dev));
	}

	return 0;
}
#elif defined(CONFIG_HAS_EARLYSUSPEND)
 /**
 * synaptics_rmi4_early_suspend()
 *
 * Called by the kernel during the early suspend phase when the system
 * enters suspend.
 *
 * This function calls synaptics_rmi4_sensor_sleep() to stop finger
 * data acquisition and put the sensor to sleep.
 */
static void synaptics_rmi4_early_suspend(struct early_suspend *h)
{
	struct synaptics_rmi4_data *rmi4_data =
			container_of(h, struct synaptics_rmi4_data,
			early_suspend);
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	if (rmi4_data->stay_awake)
		rmi4_data->staying_awake = true;
	else
		rmi4_data->staying_awake = false;

	rmi4_data->touch_stopped = true;
	wake_up(&rmi4_data->wait);
	synaptics_rmi4_irq_enable(rmi4_data, false);
	synaptics_rmi4_sensor_sleep(rmi4_data);

	if (rmi4_data->full_pm_cycle)
		synaptics_rmi4_suspend(&(rmi4_data->input_dev->dev));

	return;
}

 /**
 * synaptics_rmi4_late_resume()
 *
 * Called by the kernel during the late resume phase when the system
 * wakes up from suspend.
 *
 * This function goes through the sensor wake process if the system wakes
 * up from early suspend (without going into suspend).
 */
static void synaptics_rmi4_late_resume(struct early_suspend *h)
{
	struct synaptics_rmi4_data *rmi4_data =
			container_of(h, struct synaptics_rmi4_data,
			early_suspend);
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	if (rmi4_data->staying_awake)
		return;

	if (rmi4_data->full_pm_cycle)
		synaptics_rmi4_resume(&(rmi4_data->input_dev->dev));

	if (rmi4_data->sensor_sleep == true) {
		synaptics_rmi4_sensor_wake(rmi4_data);
		rmi4_data->touch_stopped = false;
		synaptics_rmi4_irq_enable(rmi4_data, true);
	}

	return;
}
#endif

static int synaptics_rmi4_regulator_lpm(struct synaptics_rmi4_data *rmi4_data,
						bool on)
{
	int retval;
	int load_ua;

	if (on == false)
		goto regulator_hpm;

	if (rmi4_data->board->i2c_pull_up) {
		load_ua = rmi4_data->board->power_down_enable ?
			0 : RMI4_I2C_LPM_LOAD_UA;
		retval = reg_set_optimum_mode_check(rmi4_data->vcc_i2c,
			load_ua);
		if (retval < 0) {
			dev_err(&rmi4_data->i2c_client->dev,
				"Regulator vcc_i2c set_opt failed " \
				"rc=%d\n", retval);
			goto fail_regulator_lpm;
		}

		if (rmi4_data->board->power_down_enable) {
			retval = regulator_disable(rmi4_data->vcc_i2c);
			if (retval) {
				dev_err(&rmi4_data->i2c_client->dev,
					"Regulator vcc_i2c disable failed " \
					"rc=%d\n", retval);
				goto fail_regulator_lpm;
			}
		}
	}

	load_ua = rmi4_data->board->power_down_enable ? 0 : RMI4_LPM_LOAD_UA;
	retval = reg_set_optimum_mode_check(rmi4_data->vdd, load_ua);
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
			"Regulator vdd_ana set_opt failed rc=%d\n",
			retval);
		goto fail_regulator_lpm;
	}

	if (rmi4_data->board->power_down_enable) {
		retval = regulator_disable(rmi4_data->vdd);
		if (retval) {
			dev_err(&rmi4_data->i2c_client->dev,
				"Regulator vdd disable failed rc=%d\n",
				retval);
			goto fail_regulator_lpm;
		}
	}

	return 0;

regulator_hpm:

	retval = reg_set_optimum_mode_check(rmi4_data->vdd,
				RMI4_ACTIVE_LOAD_UA);
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
			"Regulator vcc_ana set_opt failed rc=%d\n",
			retval);
		goto fail_regulator_hpm;
	}

	if (rmi4_data->board->power_down_enable) {
		retval = regulator_enable(rmi4_data->vdd);
		if (retval) {
			dev_err(&rmi4_data->i2c_client->dev,
				"Regulator vdd enable failed rc=%d\n",
				retval);
			goto fail_regulator_hpm;
		}
	}

	if (rmi4_data->board->i2c_pull_up) {
		retval = reg_set_optimum_mode_check(rmi4_data->vcc_i2c,
			RMI4_I2C_LOAD_UA);
		if (retval < 0) {
			dev_err(&rmi4_data->i2c_client->dev,
				"Regulator vcc_i2c set_opt failed rc=%d\n",
				retval);
			goto fail_regulator_hpm;
		}

		if (rmi4_data->board->power_down_enable) {
			retval = regulator_enable(rmi4_data->vcc_i2c);
			if (retval) {
				dev_err(&rmi4_data->i2c_client->dev,
					"Regulator vcc_i2c enable failed " \
					"rc=%d\n", retval);
				goto fail_regulator_hpm;
			}
		}
	}

	return 0;

fail_regulator_lpm:
	reg_set_optimum_mode_check(rmi4_data->vdd, RMI4_ACTIVE_LOAD_UA);
	if (rmi4_data->board->i2c_pull_up)
		reg_set_optimum_mode_check(rmi4_data->vcc_i2c,
						RMI4_I2C_LOAD_UA);

	return retval;

fail_regulator_hpm:
	load_ua = rmi4_data->board->power_down_enable ? 0 : RMI4_LPM_LOAD_UA;
	reg_set_optimum_mode_check(rmi4_data->vdd, load_ua);
	if (rmi4_data->board->i2c_pull_up) {
		load_ua = rmi4_data->board->power_down_enable ?
				0 : RMI4_I2C_LPM_LOAD_UA;
		reg_set_optimum_mode_check(rmi4_data->vcc_i2c, load_ua);
	}
	return retval;
}

static int synaptics_rmi4_check_configuration(struct synaptics_rmi4_data
						*rmi4_data)
{
	int retval;
	struct synaptics_rmi4_f01_device_control_0 device_control;
	struct synaptics_rmi4_f01_device_status device_status;

	retval = synaptics_rmi4_i2c_read(rmi4_data,
			rmi4_data->f01_data_base_addr,
			device_status.data,
			sizeof(device_status.data));
	if (retval < 0) {
		dev_err(&rmi4_data->i2c_client->dev,
			"Failed to read device status, rc=%d\n", retval);
		return retval;
	}

	if (device_status.unconfigured) {
		retval = synaptics_rmi4_query_device(rmi4_data);
		if (retval < 0) {
			dev_err(&rmi4_data->i2c_client->dev,
				"Failed to query device, rc=%d\n", retval);
			return retval;
		}

		retval = synaptics_rmi4_i2c_read(rmi4_data,
				rmi4_data->f01_ctrl_base_addr,
				device_control.data,
				sizeof(device_control.data));
		if (retval < 0)
			return retval;

		device_control.configured = DEVICE_CONFIGURED;

		retval = synaptics_rmi4_i2c_write(rmi4_data,
				rmi4_data->f01_ctrl_base_addr,
				device_control.data,
				sizeof(device_control.data));
		if (retval < 0)
			return retval;
	}

	return 0;
}

 /**
 * synaptics_rmi4_suspend()
 *
 * Called by the kernel during the suspend phase when the system
 * enters suspend.
 *
 * This function stops finger data acquisition and puts the sensor to
 * sleep (if not already done so during the early suspend phase),
 * disables the interrupt, and turns off the power to the sensor.
 */
#ifdef CONFIG_PM
static int synaptics_rmi4_suspend(struct device *dev)
{
	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);
	int retval;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif
	if (rmi4_data->stay_awake) {
		rmi4_data->staying_awake = true;
		return 0;
	} else
		rmi4_data->staying_awake = false;
	
	if (rmi4_data->suspended) {
		dev_info(dev, "Already in suspend state\n");
		return 0;
	}
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,PR-674715 2014/05/26, ctp enter gesture mode when suspend
	#ifdef SYNAPTICS_GESTURE_WAKE_UP
	if (g_wakeup_gesture) {
        rmi4_data->irq_enable(rmi4_data,false);
		synaptics_rmi4_wakeup_gesture(rmi4_data, true);
        rmi4_data->irq_enable(rmi4_data,true);
        enable_irq_wake(rmi4_data->irq);//add by weihong.chen 2014/07/02  ,enable intterupt when deep sleep
      goto exit;
	}
    #endif
	//[FEATURE]-Add-END by TCTSZ.weihong.chen
	if (!rmi4_data->fw_updating) {
		if (!rmi4_data->sensor_sleep) {
			rmi4_data->touch_stopped = true;
			wake_up(&rmi4_data->wait);
			synaptics_rmi4_irq_enable(rmi4_data, false);
			synaptics_rmi4_sensor_sleep(rmi4_data);
		}

		synaptics_rmi4_release_all(rmi4_data);

		retval = synaptics_rmi4_regulator_lpm(rmi4_data, true);
		if (retval < 0) {
			dev_err(dev, "failed to enter low power mode\n");
			goto err_lpm_regulator;
		}
	} else {
		dev_err(dev,
			"Firmware updating, cannot go into suspend mode\n");
		return 0;
	}

	if (rmi4_data->board->disable_gpios) {
		if (rmi4_data->ts_pinctrl) {
			retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_suspend);
			if (retval < 0)
				dev_err(dev, "failed to select idle pinctrl state\n");
		}

		retval = synaptics_rmi4_gpio_configure(rmi4_data, false);
		if (retval < 0) {
			dev_err(dev, "failed to put gpios in suspend state\n");
			goto err_gpio_configure;
		}
	}
	#ifdef SYNAPTICS_GESTURE_WAKE_UP
exit:
        #ifdef  HAVE_HALL
	hall->tp_is_suspend =1;  //add by weihong.chen 2014/07/07 ,to indicate weather call  tp_set_sensitivity()
	#endif
        #endif
	rmi4_data->suspended = true;
	#if GTP_ESD_PROTECT
	synaptics_esd_switch(rmi4_data,SWITCH_OFF);
        #endif
	return 0;

err_gpio_configure:
	if (rmi4_data->ts_pinctrl) {
		retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_active);
		if (retval < 0)
			dev_err(dev, "failed to select get default pinctrl state\n");
	}
	synaptics_rmi4_regulator_lpm(rmi4_data, false);

err_lpm_regulator:
	if (rmi4_data->sensor_sleep) {
		synaptics_rmi4_sensor_wake(rmi4_data);
		synaptics_rmi4_irq_enable(rmi4_data, true);
		rmi4_data->touch_stopped = false;
	}

	return retval;
}

 /**
 * synaptics_rmi4_resume()
 *
 * Called by the kernel during the resume phase when the system
 * wakes up from suspend.
 *
 * This function turns on the power to the sensor, wakes the sensor
 * from sleep, enables the interrupt, and starts finger data
 * acquisition.
 */
static int synaptics_rmi4_resume(struct device *dev)
{


	struct synaptics_rmi4_data *rmi4_data = dev_get_drvdata(dev);
	int retval;
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
	#endif

	if (rmi4_data->staying_awake)
		return 0;

	if (!rmi4_data->suspended) {
		dev_info(dev, "Already in awake state\n");
		return 0;
	}
	
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, ctp exit the gesture mode,and back to normal mode
	#ifdef SYNAPTICS_GESTURE_WAKE_UP
	if (g_wakeup_gesture) {
		rmi4_data->irq_enable(rmi4_data,false);
		synaptics_rmi4_wakeup_gesture(rmi4_data, false);
		rmi4_data->irq_enable(rmi4_data,true);
        disable_irq_wake(rmi4_data->irq);//add by weihong.chen 2014/07/02  ,disable deep_intterupt when the system is awake
        goto exit;
	}
    #endif
	//[FEATURE]-Add-END by TCTSZ. weihong.chen
	retval = synaptics_rmi4_regulator_lpm(rmi4_data, false);
	if (retval < 0) {
		dev_err(dev, "Failed to enter active power mode\n");
		return retval;
	}

	if (rmi4_data->board->disable_gpios) {
		if (rmi4_data->ts_pinctrl) {
			retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_active);
			if (retval < 0)
				dev_err(dev, "failed to select default pinctrl state\n");
		}

		retval = synaptics_rmi4_gpio_configure(rmi4_data, true);
		if (retval < 0) {
			dev_err(dev, "Failed to put gpios in active state\n");
			goto err_gpio_configure;
		}
	}

	synaptics_rmi4_sensor_wake(rmi4_data);
	rmi4_data->touch_stopped = false;
	synaptics_rmi4_irq_enable(rmi4_data, true);

	retval = synaptics_rmi4_check_configuration(rmi4_data);
	if (retval < 0) {
		dev_err(dev, "Failed to check configuration\n");
		goto err_check_configuration;
	}
	#ifdef SYNAPTICS_GESTURE_WAKE_UP
exit:
   
	//synaptics_is_deep_suspended = 0;
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen, 2014/07/02, clear the point when resume
    input_report_key(rmi4_data->input_dev, BTN_TOUCH, 0);
	input_sync(rmi4_data->input_dev);
	rmi4_data->enable_palm_gesture =true;
	#ifdef CONFIG_TCT_8X16_POP8LTE
	synaptics_hard_reset(rmi4_data);////[MOD]-Add-by TCTSZ. weihong.chen, 2014/08/06
	#endif
	#ifdef  HAVE_HALL
	hall->tp_is_suspend =0;  //add by weihong.chen 2014/07/07 ,to indicate weather call  tp_set_sensitivity()
	hall->tp_set_sensitivity(gpio_get_value(hall->irq_gpio));	
	#endif
	#endif
	//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen, 2014/07/02
	
	rmi4_data->suspended = false;
	
	#if GTP_ESD_PROTECT
	synaptics_esd_switch(rmi4_data,SWITCH_ON);
	#endif
	return 0;

err_check_configuration:
	synaptics_rmi4_irq_enable(rmi4_data, false);
	rmi4_data->touch_stopped = true;
	synaptics_rmi4_sensor_sleep(rmi4_data);

	if (rmi4_data->board->disable_gpios) {
		if (rmi4_data->ts_pinctrl) {
			retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_suspend);
			if (retval < 0)
				dev_err(dev, "failed to select idle pinctrl state\n");
		}

		synaptics_rmi4_gpio_configure(rmi4_data, false);
	}
	synaptics_rmi4_regulator_lpm(rmi4_data, true);
	wake_up(&rmi4_data->wait);

	return retval;

err_gpio_configure:
	if (rmi4_data->ts_pinctrl) {
		retval = pinctrl_select_state(rmi4_data->ts_pinctrl,
					rmi4_data->pinctrl_state_suspend);
		if (retval < 0)
			pr_err("failed to select idle pinctrl state\n");
	}
	synaptics_rmi4_regulator_lpm(rmi4_data, true);
	wake_up(&rmi4_data->wait);

	return retval;


}

#if (!defined(CONFIG_FB) && !defined(CONFIG_HAS_EARLYSUSPEND))
static const struct dev_pm_ops synaptics_rmi4_dev_pm_ops = {
	.suspend = synaptics_rmi4_suspend
	.resume  = synaptics_rmi4_resume,
};
#else
//[FEATURE]-Add-BEGIN by TCTSZ. weihong.chen,FR-674715 2014/05/26, do not do the work on deep sleep,do it on resume
#ifdef SYNAPTICS_GESTURE_WAKE_UP
static int synaptics_deep_suspend(struct device *dev)
{  
    #ifdef TP_DEBUG
	STP_DEBUG_FUNC();
    #endif

	synaptics_is_deep_suspended = 1;
	return 0;
}

static int synaptics_deep_resume(struct device *dev)
{
	#ifdef TP_DEBUG
	STP_DEBUG_FUNC();
	#endif

//	queue_work(rmi4_data->synaptics_wq, &rmi4_data->work);
	synaptics_is_deep_suspended = 0;

	return 0;
}
#endif

static const struct dev_pm_ops synaptics_rmi4_dev_pm_ops = {
#ifdef SYNAPTICS_GESTURE_WAKE_UP
    .suspend = synaptics_deep_suspend,
	.resume = synaptics_deep_resume,
#endif
//[FEATURE]-Add-END by TCTSZ. weihong.chen

};

#endif
#else
static int synaptics_rmi4_suspend(struct device *dev)
{
	return 0;
}

static int synaptics_rmi4_resume(struct device *dev)
{
	return 0;
}
#endif

static const struct i2c_device_id synaptics_rmi4_id_table[] = {
	{DRIVER_NAME, 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, synaptics_rmi4_id_table);

#ifdef CONFIG_OF
static struct of_device_id rmi4_match_table[] = {
	{ .compatible = "synaptics,rmi4",},
	{ },
};
#else
#define rmi4_match_table NULL
#endif

static struct i2c_driver synaptics_rmi4_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = rmi4_match_table,
#ifdef CONFIG_PM
		.pm = &synaptics_rmi4_dev_pm_ops,
#endif
	},
	.probe = synaptics_rmi4_probe,
	.remove = synaptics_rmi4_remove,
	.id_table = synaptics_rmi4_id_table,
};

 /**
 * synaptics_rmi4_init()
 *
 * Called by the kernel during do_initcalls (if built-in)
 * or when the driver is loaded (if a module).
 *
 * This function registers the driver to the I2C subsystem.
 *
 */
static int __init synaptics_rmi4_init(void)
{
	return i2c_add_driver(&synaptics_rmi4_driver);
}

 /**
 * synaptics_rmi4_exit()
 *
 * Called by the kernel when the driver is unloaded.
 *
 * This funtion unregisters the driver from the I2C subsystem.
 *
 */
static void __exit synaptics_rmi4_exit(void)
{
	i2c_del_driver(&synaptics_rmi4_driver);
}
#ifdef  CONFIG_TCT_8X16_POP8LTE
late_initcall(synaptics_rmi4_init);
#else
module_initcall(synaptics_rmi4_init);
#endif
module_exit(synaptics_rmi4_exit);

MODULE_AUTHOR("Synaptics, Inc.");
MODULE_DESCRIPTION("Synaptics RMI4 I2C Touch Driver");
MODULE_LICENSE("GPL v2");