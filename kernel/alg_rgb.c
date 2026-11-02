// SPDX-License-Identifier: GPL-2.0
/*
 * alg_rgb.c - Acer ALG (CLV0001) RGB keyboard driver
 *
 * The firmware interface was reverse engineered from Acer's Windows
 * AcpiBridge stack.  Windows talks to the CLV0001 ACPI device with a
 * _DSM call using UUID 93f224e4-fbdc-4bbf-add6-db71bdc0afad and function
 * 0x67.  The function-specific argument is a one-element package holding
 * a four byte buffer:
 *
 *     byte 0 = green
 *     byte 1 = red
 *     byte 2 = blue
 *     byte 3 = zone / command
 *
 * IMPORTANT SAFETY NOTE
 * ---------------------
 * Only function 0x67 is implemented.  Functions 0x68, 0x69 and 0x6A are
 * never called.  An experimental call in that range hard-locked an
 * AL15G-53 during reverse engineering.  Do not add speculative ACPI
 * probes here.
 *
 * Userspace interface
 * -------------------
 *   /dev/alg_rgb          line-oriented command device, read returns state
 *   /sys/class/misc/alg_rgb/state          read-only state snapshot
 *   /sys/class/misc/alg_rgb/capabilities   verified protocol facts
 *
 * Supported commands (one line per write):
 *
 *   color R G B [ZONE]   apply RGB (0-255) to a zone (default 0xF0)
 *   frame R G B [ZONE]   send one raw animation frame (no state change)
 *   brightness N         set firmware brightness 0..4
 *   effect NAME          random|flash|tempo|dance|wave|cycle|breath|none
 *   off                  black out the current zone
 *   on                   restore the last colour / effect and brightness
 *   zone ZONE            change the default zone without writing LEDs
 *   reset                return to the built-in safe default
 *
 * This driver is intentionally small.  All policy, persistence and UI
 * live in userspace so the ACPI surface stays auditable.
 */

#include <linux/acpi.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uuid.h>

#include "../include/version.h"
#include "../include/protocol.h"

#define DEVICE_NAME "alg_rgb"
#define ALG_MAX_COMMAND 128
#define ALG_STATUS_LEN 192

static guid_t alg_rgb_guid = GUID_INIT(
	0x93f224e4,
	0xfbdc,
	0x4bbf,
	0xad, 0xd6, 0xdb, 0x71, 0xbd, 0xc0, 0xaf, 0xad);

static acpi_handle alg_acpi_handle;
static DEFINE_MUTEX(alg_lock);

struct alg_effect {
	const char *name;
	u8 packet[4];
};

/*
 * All effect packets were captured from Acer's Windows keyboard module.
 * They are sent through the same verified _DSM 0x67 path as colours.
 */
static const struct alg_effect alg_effects[] = {
	{ "random", { 0x00, 0x00, 0x00, 0x70 } },
	{ "flash",  { 0x00, 0x00, 0x00, 0xA0 } },
	{ "tempo",  { 0x00, 0x00, 0x00, 0x90 } },
	{ "dance",  { 0x00, 0x00, 0x00, 0x80 } },
	{ "wave",   { 0x00, 0x00, 0x00, 0xB0 } },
	{ "cycle",  { 0x00, 0x00, 0x01, 0x33 } },
	{ "breath", { 0x00, 0xA0, 0x02, 0x10 } },
};

struct alg_state {
	u8 r;
	u8 g;
	u8 b;
	u8 brightness;
	u8 zone;
	bool power;
	const struct alg_effect *effect;
};

static struct alg_state alg_state = {
	.r = 255,
	.g = 255,
	.b = 255,
	.brightness = 2,
	.zone = ALG_ZONE_MAIN,
	.power = true,
	.effect = NULL,
};

/* -------------------------------------------------------------------------- */
/* ACPI                                                                       */
/* -------------------------------------------------------------------------- */

static acpi_status alg_acpi_match(acpi_handle handle, u32 level, void *ctx,
				  void **retval)
{
	acpi_handle *out = ctx;

	if (!out)
		return AE_BAD_PARAMETER;

	*out = handle;
	return AE_CTRL_TERMINATE;
}

static int alg_find_acpi_device(void)
{
	acpi_status status;

	/*
	 * The AL15G-52 exposes CLV0001 under \_SB.DCHU.  Try the known
	 * path first, then fall back to a HID scan so the driver also works
	 * on machines where the namespace path differs.
	 */
	status = acpi_get_handle(NULL, "\\_SB.DCHU", &alg_acpi_handle);
	if (ACPI_SUCCESS(status) && alg_acpi_handle)
		return 0;

	alg_acpi_handle = NULL;
	status = acpi_get_devices("CLV0001", alg_acpi_match, &alg_acpi_handle,
				  NULL);
	if (ACPI_SUCCESS(status) && alg_acpi_handle)
		return 0;

	alg_acpi_handle = NULL;
	return -ENODEV;
}

/*
 * Send one verified 4-byte packet through _DSM 0x67.  The caller must hold
 * alg_lock.  The ACPI buffer has to stay alive until acpi_evaluate_dsm()
 * returns, so it is allocated from the heap rather than placed on the
 * stack.
 */
static int alg_send_packet(const u8 packet[4])
{
	union acpi_object *result;
	union acpi_object buffer_obj;
	union acpi_object package_obj;
	u8 *buffer;
	int ret = 0;

	if (!alg_acpi_handle)
		return -ENODEV;

	buffer = kmemdup(packet, 4, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer_obj.type = ACPI_TYPE_BUFFER;
	buffer_obj.buffer.length = 4;
	buffer_obj.buffer.pointer = buffer;

	package_obj.type = ACPI_TYPE_PACKAGE;
	package_obj.package.count = 1;
	package_obj.package.elements = &buffer_obj;

	result = acpi_evaluate_dsm(alg_acpi_handle, &alg_rgb_guid, 0,
				   ALG_DSM_FUNCTION, &package_obj);
	if (!result) {
		/*
		 * A NULL result means the AML method itself could not be
		 * evaluated.  That is a real transport failure.
		 */
		pr_err_ratelimited("alg-rgb: _DSM 0x%02x returned no result\n",
				   ALG_DSM_FUNCTION);
		ret = -EIO;
	} else {
		/*
		 * CLV0001 firmware on the AL15G-52 returns a non-zero
		 * integer status even when it successfully applies the
		 * packet (confirmed on hardware: a red command with a
		 * non-zero status visibly changed the keyboard).  Do not
		 * interpret that vendor status as a failure.  Only a NULL
		 * evaluation means the command was not delivered.
		 */
		if (result->type == ACPI_TYPE_INTEGER &&
		    result->integer.value != 0)
			pr_info_once("alg-rgb: firmware _DSM status 0x%llx (applied)\n",
				     result->integer.value);
		ACPI_FREE(result);
	}

	kfree(buffer);
	return ret;
}

static int alg_send_color(u8 r, u8 g, u8 b, u8 zone)
{
	/* Firmware order is GRB, not RGB. */
	u8 packet[4] = { g, r, b, zone };

	return alg_send_packet(packet);
}

/*
 * Firmware brightness steps captured from the vendor stack.  The command
 * takes the raw step value in the first payload byte, not the 0..4 index
 * used by the userspace interface.
 */
static const u8 alg_brightness_levels[ALG_BRIGHTNESS_MAX + 1] = {
	0x00, 0x2F, 0x5F, 0x8F, 0xBF,
};

static int alg_send_brightness(u8 level)
{
	u8 packet[4];

	if (level > ALG_BRIGHTNESS_MAX)
		return -EINVAL;

	packet[0] = alg_brightness_levels[level];
	packet[1] = 0;
	packet[2] = 0;
	packet[3] = ALG_ZONE_BRIGHTNESS;

	return alg_send_packet(packet);
}

/* -------------------------------------------------------------------------- */
/* State helpers (caller holds alg_lock)                                      */
/* -------------------------------------------------------------------------- */

static bool alg_zone_valid(u8 zone)
{
	switch (zone) {
	case ALG_ZONE_MAIN:
	case ALG_ZONE_MID:
	case ALG_ZONE_RIGHT:
	case ALG_ZONE_BAR:
	case ALG_ZONE_LOGO:
		return true;
	default:
		return false;
	}
}

static const struct alg_effect *alg_effect_by_name(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(alg_effects); i++) {
		if (!strcmp(name, alg_effects[i].name))
			return &alg_effects[i];
	}

	return NULL;
}

static int alg_push_color_locked(void)
{
	int ret;

	ret = alg_send_color(alg_state.r, alg_state.g, alg_state.b,
			     alg_state.zone);
	if (ret)
		return ret;

	if (alg_state.power)
		ret = alg_send_brightness(alg_state.brightness);

	return ret;
}

static int alg_push_effect_locked(void)
{
	int ret;

	if (!alg_state.effect)
		return alg_push_color_locked();

	ret = alg_send_packet(alg_state.effect->packet);
	if (ret)
		return ret;

	if (alg_state.power)
		ret = alg_send_brightness(alg_state.brightness);

	return ret;
}

static int alg_push_current_locked(void)
{
	if (alg_state.effect)
		return alg_push_effect_locked();

	return alg_push_color_locked();
}

static int alg_set_power_locked(bool on)
{
	int ret;

	if (!on) {
		u8 black[4] = { 0x00, 0x00, 0x00, alg_state.zone };

		ret = alg_send_packet(black);
		if (ret)
			return ret;

		/* Keep the saved brightness for the next "on". */
		ret = alg_send_brightness(0);
		if (ret)
			return ret;

		alg_state.power = false;
		return 0;
	}

	if (alg_state.brightness == 0)
		alg_state.brightness = 3;

	alg_state.power = true;
	return alg_push_current_locked();
}

/* -------------------------------------------------------------------------- */
/* Command parser (caller holds alg_lock)                                     */
/* -------------------------------------------------------------------------- */

static int alg_command_color(char *args)
{
	unsigned int r, g, b;
	unsigned int zone = alg_state.zone;
	int fields;
	int ret;

	fields = sscanf(args, "%u %u %u %x", &r, &g, &b, &zone);
	if (fields < 3)
		return -EINVAL;
	if (r > 255 || g > 255 || b > 255)
		return -EINVAL;
	if (!alg_zone_valid((u8)zone))
		return -EINVAL;

	alg_state.r = (u8)r;
	alg_state.g = (u8)g;
	alg_state.b = (u8)b;
	alg_state.zone = (u8)zone;
	alg_state.effect = NULL;
	alg_state.power = true;

	/* Picking a colour while dimmed should produce visible light. */
	if (alg_state.brightness == 0)
		alg_state.brightness = 3;

	ret = alg_push_color_locked();
	return ret;
}

static int alg_command_brightness(char *args)
{
	unsigned int level;
	bool was_off;
	int ret;

	if (sscanf(args, "%u", &level) != 1 || level > ALG_BRIGHTNESS_MAX)
		return -EINVAL;

	was_off = !alg_state.power;
	alg_state.brightness = (u8)level;
	alg_state.power = level > 0;

	if (level == 0) {
		ret = alg_send_brightness(0);
	} else if (was_off) {
		/* Re-apply the last colour/effect before raising brightness. */
		ret = alg_push_current_locked();
	} else {
		ret = alg_send_brightness((u8)level);
	}

	return ret;
}

static int alg_command_effect(char *args)
{
	const struct alg_effect *effect;
	int ret;

	args = strim(args);
	if (!*args)
		return -EINVAL;

	if (!strcmp(args, "none") || !strcmp(args, "static")) {
		alg_state.effect = NULL;
		alg_state.power = true;
		if (alg_state.brightness == 0)
			alg_state.brightness = 3;
		return alg_push_color_locked();
	}

	effect = alg_effect_by_name(args);
	if (!effect)
		return -EINVAL;

	alg_state.effect = effect;
	alg_state.power = true;
	if (alg_state.brightness == 0)
		alg_state.brightness = 3;

	ret = alg_push_effect_locked();
	return ret;
}

static int alg_command_zone(char *args)
{
	unsigned int zone;

	if (sscanf(args, "%x", &zone) != 1)
		return -EINVAL;
	if (!alg_zone_valid((u8)zone))
		return -EINVAL;

	alg_state.zone = (u8)zone;
	return 0;
}

/*
 * One raw animation frame.  This deliberately does not touch alg_state or
 * the persistence files: it is a low-overhead write used by the userspace
 * animation daemon, while the saved state stays on the static base colour.
 * The caller has already enabled the controller with a normal colour write.
 */
static int alg_command_frame(char *args)
{
	unsigned int r, g, b;
	unsigned int zone = alg_state.zone;
	int fields;

	fields = sscanf(args, "%u %u %u %x", &r, &g, &b, &zone);
	if (fields < 3)
		return -EINVAL;
	if (r > 255 || g > 255 || b > 255)
		return -EINVAL;
	if (!alg_zone_valid((u8)zone))
		return -EINVAL;

	return alg_send_color((u8)r, (u8)g, (u8)b, (u8)zone);
}

static int alg_command_reset(void)
{
	alg_state.r = 255;
	alg_state.g = 255;
	alg_state.b = 255;
	alg_state.brightness = 2;
	alg_state.zone = ALG_ZONE_MAIN;
	alg_state.power = true;
	alg_state.effect = NULL;

	return alg_push_color_locked();
}

static int alg_handle_command(char *command)
{
	if (!strcmp(command, "off"))
		return alg_set_power_locked(false);

	if (!strcmp(command, "on"))
		return alg_set_power_locked(true);

	if (!strcmp(command, "reset"))
		return alg_command_reset();

	if (!strcmp(command, "status"))
		return 0;

	if (!strncmp(command, "color ", 6))
		return alg_command_color(command + 6);

	if (!strncmp(command, "frame ", 6))
		return alg_command_frame(command + 6);

	if (!strncmp(command, "brightness ", 11))
		return alg_command_brightness(command + 11);

	if (!strncmp(command, "effect ", 7))
		return alg_command_effect(command + 7);

	if (!strncmp(command, "zone ", 5))
		return alg_command_zone(command + 5);

	return -EINVAL;
}

/* -------------------------------------------------------------------------- */
/* Character device                                                           */
/* -------------------------------------------------------------------------- */

static int alg_format_state(char *buf, size_t len)
{
	const char *effect = alg_state.effect ? alg_state.effect->name : "none";

	return scnprintf(
		buf, len,
		"version=%s;power=%s;color=#%02x%02x%02x;brightness=%u;effect=%s;zone=0x%02x\n",
		ALG_RGB_VERSION_STRING, alg_state.power ? "on" : "off",
		alg_state.r, alg_state.g, alg_state.b, alg_state.brightness,
		effect, alg_state.zone);
}

static ssize_t alg_read(struct file *file, char __user *user_buf,
			size_t count, loff_t *ppos)
{
	char status[ALG_STATUS_LEN];
	int len;

	if (mutex_lock_interruptible(&alg_lock))
		return -ERESTARTSYS;

	len = alg_format_state(status, sizeof(status));
	mutex_unlock(&alg_lock);

	return simple_read_from_buffer(user_buf, count, ppos, status, len);
}

static ssize_t alg_write(struct file *file, const char __user *user_buf,
			 size_t count, loff_t *ppos)
{
	char command[ALG_MAX_COMMAND];
	char *newline;
	struct alg_state previous;
	int ret;

	if (count == 0 || count >= sizeof(command))
		return -EINVAL;

	if (copy_from_user(command, user_buf, count))
		return -EFAULT;

	command[count] = '\0';

	/* One command per write; ignore anything after the first newline. */
	newline = strpbrk(command, "\r\n");
	if (newline)
		*newline = '\0';

	strim(command);
	if (!*command)
		return -EINVAL;

	if (mutex_lock_interruptible(&alg_lock))
		return -ERESTARTSYS;

	previous = alg_state;
	ret = alg_handle_command(command);
	if (ret < 0)
		alg_state = previous;
	mutex_unlock(&alg_lock);

	if (ret < 0)
		return ret;

	return count;
}

static const struct file_operations alg_fops = {
	.owner = THIS_MODULE,
	.read = alg_read,
	.write = alg_write,
	.llseek = noop_llseek,
};

/* -------------------------------------------------------------------------- */
/* sysfs                                                                      */
/* -------------------------------------------------------------------------- */

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	int len;

	if (mutex_lock_interruptible(&alg_lock))
		return -ERESTARTSYS;

	len = alg_format_state(buf, PAGE_SIZE);
	mutex_unlock(&alg_lock);

	return len;
}
static DEVICE_ATTR_RO(state);

static ssize_t capabilities_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(
		buf,
		"protocol=acpi-dsm\n"
		"uuid=" ALG_DSM_UUID_STR "\n"
		"function=0x%02x\n"
		"color_order=GRB\n"
		"zones=0xF0,0xF1,0xF2,0xF3,0xF6\n"
		"brightness_levels=0x00,0x2F,0x5F,0x8F,0xBF\n"
		"effects=random,flash,tempo,dance,wave,cycle,breath\n"
		"animation=software-frames\n"
		"perkey_hid=048D:8910\n"
		"perkey=unavailable\n"
		"safety=verified-dsm-0x67-only\n",
		ALG_DSM_FUNCTION);
}
static DEVICE_ATTR_RO(capabilities);

static struct attribute *alg_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_capabilities.attr,
	NULL,
};
ATTRIBUTE_GROUPS(alg);

static struct miscdevice alg_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &alg_fops,
	.mode = 0660,
	.groups = alg_groups,
};

/* -------------------------------------------------------------------------- */
/* Module                                                                     */
/* -------------------------------------------------------------------------- */

static int __init alg_init(void)
{
	int ret;

	pr_info("alg-rgb: loading v%s\n", ALG_RGB_VERSION_STRING);

	if (alg_find_acpi_device())
		pr_warn("alg-rgb: CLV0001 not found; hardware writes will fail\n");
	else
		pr_info("alg-rgb: found CLV0001 at %s\n", "\\_SB.DCHU");

	ret = misc_register(&alg_misc);
	if (ret) {
		pr_err("alg-rgb: misc_register failed: %d\n", ret);
		return ret;
	}

	pr_info("alg-rgb: ready at /dev/%s\n", DEVICE_NAME);
	return 0;
}

static void __exit alg_exit(void)
{
	misc_deregister(&alg_misc);
	pr_info("alg-rgb: unloaded\n");
}

module_init(alg_init);
module_exit(alg_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kaushik Sarkar; AL15G-52 adaptation and extensions");
MODULE_DESCRIPTION("Acer ALG CLV0001 RGB keyboard driver (verified _DSM 0x67 only)");
MODULE_VERSION(ALG_RGB_VERSION_STRING);
MODULE_ALIAS("acpi:CLV0001");
