// SPDX-License-Identifier: GPL-2.0
/*
 * alg-rgb - command line control for the Acer ALG CLV0001 RGB keyboard
 *
 * The CLI is deliberately a thin client of /dev/alg_rgb.  It validates
 * names and colours, performs safe persistence, and can produce JSON for
 * the GNOME extension and the local web dashboard.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../include/version.h"
#include "../include/protocol.h"

#define DEFAULT_DEVICE "/dev/alg_rgb"
#define STATE_SYSTEM "/var/lib/alg-rgb/state"
#define STATE_DIR "/var/lib/alg-rgb"
#define CAPABILITIES_PATH "/sys/class/misc/alg_rgb/capabilities"
#define MODULE_VERSION_PATH "/sys/module/alg_rgb/version"
#define ACPI_DEVICE_PATH "/sys/bus/acpi/devices/CLV0001:00"

#define ANIMATION_DELAY_DEFAULT 100
#define ANIMATION_DELAY_MIN 50
#define ANIMATION_DELAY_MAX 2000
#define ANIMATION_WAIT_MS 2000
#define ANIMATION_LOCK_NAME "alg-rgb-animation.lock"
#define ANIMATION_EFFECT_MAX 32

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_HID_DEVICES 32
#define HID_NAME_LEN 128

struct named_color {
	const char *name;
	unsigned int r;
	unsigned int g;
	unsigned int b;
};

/*
 * Values are in normal RGB order.  The kernel converts to the GRB order
 * expected by the firmware.  These are the same presets Acer's Windows
 * application exposes, plus off and white.
 */
static const struct named_color named_colors[] = {
	{ "off",           0x00, 0x00, 0x00 },
	{ "red",           0xFF, 0x00, 0x00 },
	{ "orange",        0xFF, 0x7F, 0x00 },
	{ "yellow",        0xFF, 0xFF, 0x00 },
	{ "lime",          0x7F, 0xFF, 0x00 },
	{ "light-green",   0x3F, 0xFF, 0x00 },
	{ "green",         0x00, 0xFF, 0x00 },
	{ "green-cyan",    0x00, 0xFF, 0x46 },
	{ "cyan",          0x00, 0xFF, 0xC8 },
	{ "light-blue",    0x00, 0x7F, 0xC8 },
	{ "blue",          0x00, 0x00, 0xC8 },
	{ "violet",        0x7F, 0x00, 0xC8 },
	{ "magenta",       0xFF, 0x00, 0xC8 },
	{ "pink",          0xFF, 0x00, 0x7F },
	{ "flesh",         0xFF, 0x7F, 0x3F },
	{ "bluish-white",  0x3F, 0x7F, 0x7F },
	{ "white",         0xFF, 0xFF, 0xFF },
};

struct named_effect {
	const char *name;
	const char *description;
};

static const struct named_effect named_effects[] = {
	{ "none",   "Static colour (no firmware animation)" },
	{ "random", "Random colour changes" },
	{ "flash",  "Flashing keys" },
	{ "tempo",  "Tempo / beat animation" },
	{ "dance",  "Dancing colour pattern" },
	{ "wave",   "Wave sweeping across the keyboard" },
	{ "cycle",  "Spectrum colour cycle" },
	{ "breath", "Breathing fade" },
};

struct named_zone {
	const char *name;
	unsigned int id;
	const char *description;
};

static const struct named_zone named_zones[] = {
	{ "main",  ALG_ZONE_MAIN,  "Keyboard / main zone (default)" },
	{ "left",  ALG_ZONE_LEFT,  "Left keyboard zone (same as main on AL15G-52)" },
	{ "mid",   ALG_ZONE_MID,   "Middle keyboard zone" },
	{ "right", ALG_ZONE_RIGHT, "Right keyboard zone" },
	{ "bar",   ALG_ZONE_BAR,   "Light bar" },
	{ "logo",  ALG_ZONE_LOGO,  "Logo LED" },
};

struct status {
	bool ok;
	char version[32];
	char power[8];
	char color[16];
	char effect[32];
	char zone[16];
	int brightness;
	unsigned int r;
	unsigned int g;
	unsigned int b;
};

struct options {
	bool json;
	const char *device;
	const char *zone;
	const char *file;
	int brightness;
	bool have_brightness;
	bool system;
};

struct animation_state {
	pid_t pid;
	char effect[ANIMATION_EFFECT_MAX];
	char color[16];
	unsigned int zone;
	int delay_ms;
};

static const char *g_device = DEFAULT_DEVICE;
static char g_error[512];
static volatile sig_atomic_t animation_running = 1;


static void set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_error, sizeof(g_error), fmt, ap);
	va_end(ap);
}

static void json_string(FILE *out, const char *text)
{
	const unsigned char *p;

	fputc('"', out);
	for (p = (const unsigned char *)text; *p; p++) {
		switch (*p) {
		case '"':
			fputs("\\\"", out);
			break;
		case '\\':
			fputs("\\\\", out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\r':
			fputs("\\r", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		default:
			if (*p < 0x20)
				fprintf(out, "\\u%04x", *p);
			else
				fputc(*p, out);
		}
	}
	fputc('"', out);
}

static void print_error(bool json, const char *fmt, ...)
{
	char message[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	if (json) {
		fputs("{\"ok\":false,\"error\":", stdout);
		json_string(stdout, message);
		fputs("}\n", stdout);
	} else {
		fprintf(stderr, "alg-rgb: %s\n", message);
	}
}

static const char *zone_name(unsigned int id)
{
	size_t i;

	if (id == 0)
		return "all";

	for (i = 0; i < ARRAY_SIZE(named_zones); i++) {
		if (named_zones[i].id == id)
			return named_zones[i].name;
	}

	return "unknown";
}

static int parse_zone(const char *text, unsigned int *id)
{
	char *end = NULL;
	unsigned long value;
	size_t i;

	if (!text || !*text)
		return -1;

	if (!strcasecmp(text, "all")) {
		*id = 0;
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(named_zones); i++) {
		if (!strcasecmp(text, named_zones[i].name)) {
			*id = named_zones[i].id;
			return 0;
		}
	}

	errno = 0;
	value = strtoul(text, &end, 0);
	if (errno || !end || *end != '\0' || value > 0xFF)
		return -1;

	if (value == 0) {
		*id = 0;
		return 0;
	}

	switch (value) {
	case ALG_ZONE_MAIN:
	case ALG_ZONE_MID:
	case ALG_ZONE_RIGHT:
	case ALG_ZONE_BAR:
	case ALG_ZONE_LOGO:
		*id = (unsigned int)value;
		return 0;
	default:
		return -1;
	}
}

static int parse_color(const char *text, unsigned int *r, unsigned int *g,
		       unsigned int *b)
{
	const char *hex;
	char *end = NULL;
	unsigned long value;
	size_t i;

	if (!text || !*text)
		return -1;

	for (i = 0; i < ARRAY_SIZE(named_colors); i++) {
		if (!strcasecmp(text, named_colors[i].name)) {
			*r = named_colors[i].r;
			*g = named_colors[i].g;
			*b = named_colors[i].b;
			return 0;
		}
	}

	hex = text;
	if (hex[0] == '#')
		hex++;
	if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
		hex += 2;
	if (strlen(hex) != 6)
		return -1;

	errno = 0;
	value = strtoul(hex, &end, 16);
	if (errno || !end || *end != '\0' || value > 0xFFFFFF)
		return -1;

	*r = (value >> 16) & 0xFF;
	*g = (value >> 8) & 0xFF;
	*b = value & 0xFF;
	return 0;
}

static int parse_brightness(const char *text, int *level)
{
	char *end = NULL;
	long value;

	if (!text || !*text)
		return -1;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || !end || *end != '\0')
		return -1;
	if (value < 0 || value > ALG_BRIGHTNESS_MAX)
		return -1;

	*level = (int)value;
	return 0;
}

static int effect_exists(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(named_effects); i++) {
		if (!strcasecmp(name, named_effects[i].name))
			return 0;
	}

	/* "static" is a friendly alias for no effect. */
	if (!strcasecmp(name, "static"))
		return 0;

	return -1;
}

static void canonical_effect(const char *input, char *out, size_t cap)
{
	if (!strcasecmp(input, "static")) {
		snprintf(out, cap, "none");
		return;
	}

	snprintf(out, cap, "%s", input);
}

static int read_text_file(const char *path, char *buf, size_t cap)
{
	FILE *file;
	size_t length;

	file = fopen(path, "r");
	if (!file)
		return -1;

	length = fread(buf, 1, cap - 1, file);
	fclose(file);
	buf[length] = '\0';

	while (length > 0 &&
	       (buf[length - 1] == '\n' || buf[length - 1] == '\r' ||
		buf[length - 1] == ' ' || buf[length - 1] == '\t'))
		buf[--length] = '\0';

	return 0;
}

static int status_load(struct status *st);
static int state_save_to(const char *path, const struct status *st);

static int read_state_raw(char *buf, size_t cap)
{
	int fd;
	ssize_t length;

	fd = open(g_device, O_RDONLY);
	if (fd < 0) {
		set_error("cannot open %s: %s", g_device, strerror(errno));
		return -1;
	}

	length = read(fd, buf, cap - 1);
	if (length < 0) {
		set_error("cannot read %s: %s", g_device, strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);

	if (length == 0) {
		set_error("empty response from %s", g_device);
		return -1;
	}

	buf[length] = '\0';
	return 0;
}

static int write_command(const char *fmt, ...)
{
	char command[256];
	va_list ap;
	int length;
	int fd;
	ssize_t written;

	va_start(ap, fmt);
	length = vsnprintf(command, sizeof(command), fmt, ap);
	va_end(ap);

	if (length < 0 || (size_t)length >= sizeof(command)) {
		set_error("internal command too long");
		return -1;
	}

	fd = open(g_device, O_WRONLY);
	if (fd < 0) {
		set_error("cannot open %s: %s", g_device, strerror(errno));
		return -1;
	}

	written = write(fd, command, (size_t)length);
	close(fd);

	if (written < 0) {
		set_error("write to %s failed: %s", g_device, strerror(errno));
		return -1;
	}
	if (written != length) {
		set_error("short write to %s (%zd of %d bytes)", g_device,
			  written, length);
		return -1;
	}

	/*
	 * Read back the state.  This catches firmware rejections where the
	 * write itself succeeded but no state change happened.
	 */
	if (status_load(NULL) < 0)
		return -1;

	return 0;
}

static int get_field(const char *buf, const char *key, char *out, size_t cap)
{
	size_t key_len = strlen(key);
	const char *p = buf;

	while (*p) {
		if ((p == buf || p[-1] == ';') &&
		    !strncmp(p, key, key_len) && p[key_len] == '=') {
			const char *value = p + key_len + 1;
			size_t i = 0;

			while (*value && *value != ';' && *value != '\n' &&
			       i + 1 < cap)
				out[i++] = *value++;
			out[i] = '\0';
			return 0;
		}
		p++;
	}

	return -1;
}

static int status_load(struct status *st)
{
	char raw[512];
	char brightness[32];
	struct status parsed;
	unsigned int r, g, b;

	if (read_state_raw(raw, sizeof(raw)) < 0)
		return -1;

	memset(&parsed, 0, sizeof(parsed));

	if (get_field(raw, "version", parsed.version,
		      sizeof(parsed.version)) < 0 ||
	    get_field(raw, "power", parsed.power, sizeof(parsed.power)) < 0 ||
	    get_field(raw, "color", parsed.color, sizeof(parsed.color)) < 0 ||
	    get_field(raw, "brightness", brightness, sizeof(brightness)) < 0 ||
	    get_field(raw, "effect", parsed.effect, sizeof(parsed.effect)) < 0 ||
	    get_field(raw, "zone", parsed.zone, sizeof(parsed.zone)) < 0) {
		set_error("unrecognised status response from %s", g_device);
		return -1;
	}

	if (parse_brightness(brightness, &parsed.brightness) < 0) {
		set_error("invalid brightness in status response");
		return -1;
	}

	if (parse_color(parsed.color, &r, &g, &b) < 0) {
		set_error("invalid colour in status response");
		return -1;
	}
	parsed.r = r;
	parsed.g = g;
	parsed.b = b;
	parsed.ok = true;

	if (st)
		*st = parsed;

	return 0;
}

static void status_fields_json(FILE *out, const struct status *st)
{
	unsigned int zone = 0;

	fputs("\"version\":", out);
	json_string(out, st->version);
	fputs(",\"power\":", out);
	json_string(out, st->power);
	fputs(",\"color\":", out);
	json_string(out, st->color);
	fprintf(out, ",\"brightness\":%d", st->brightness);
	fputs(",\"effect\":", out);
	json_string(out, st->effect);
	fputs(",\"zone\":", out);
	json_string(out, st->zone);
	if (!strcasecmp(st->zone, "all"))
		zone = 0;
	else
		sscanf(st->zone, "%x", &zone);
	fputs(",\"zone_name\":", out);
	json_string(out, zone_name(zone));
	fprintf(out, ",\"r\":%u,\"g\":%u,\"b\":%u", st->r, st->g, st->b);
}

static void status_json(FILE *out, const struct status *st)
{
	fputs("{\"ok\":true,", out);
	status_fields_json(out, st);
	fputs("}\n", out);
}

static void status_human(const struct status *st, bool verbose)
{
	unsigned int zone = 0;

	if (!strcasecmp(st->zone, "all"))
		zone = 0;
	else
		sscanf(st->zone, "%x", &zone);

	if (!verbose) {
		printf("Keyboard: %s | color %s | brightness %d/%d | effect %s | zone %s\n",
		       st->power, st->color, st->brightness, ALG_BRIGHTNESS_MAX,
		       st->effect, zone_name(zone));
		return;
	}

	printf("Power:      %s\n", st->power);
	printf("Color:      %s (R=%u G=%u B=%u)\n", st->color, st->r, st->g,
	       st->b);
	printf("Brightness: %d/%d\n", st->brightness, ALG_BRIGHTNESS_MAX);
	printf("Effect:     %s\n", st->effect);
	printf("Zone:       %s (%s)\n", zone_name(zone), st->zone);
	printf("Driver:     %s\n", st->version);
}

static int mkdir_p(const char *path)
{
	char tmp[1024];
	size_t length;
	char *p;

	if (!path || !*path)
		return -1;
	if (strlen(path) >= sizeof(tmp)) {
		set_error("path too long");
		return -1;
	}

	strcpy(tmp, path);
	length = strlen(tmp);
	while (length > 1 && tmp[length - 1] == '/')
		tmp[--length] = '\0';

	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
			set_error("cannot create %s: %s", tmp, strerror(errno));
			return -1;
		}
		*p = '/';
	}

	if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
		set_error("cannot create %s: %s", tmp, strerror(errno));
		return -1;
	}

	return 0;
}

static int user_state_path(char *buf, size_t cap)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");

	if (xdg && *xdg) {
		snprintf(buf, cap, "%s/alg-rgb/state", xdg);
		return 0;
	}
	if (home && *home) {
		snprintf(buf, cap, "%s/.config/alg-rgb/state", home);
		return 0;
	}

	set_error("cannot determine home directory for state file");
	return -1;
}

static int state_save_to(const char *path, const struct status *st)
{
	char directory[1024];
	char temp[1200];
	const char *slash;
	FILE *file;
	size_t length;

	slash = strrchr(path, '/');
	if (slash) {
		length = (size_t)(slash - path);
		if (length == 0)
			length = 1; /* root directory */
		if (length >= sizeof(directory)) {
			set_error("state path too long");
			return -1;
		}
		memcpy(directory, path, length);
		directory[length] = '\0';
		if (mkdir_p(directory) < 0)
			return -1;
	}

	snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());

	file = fopen(temp, "w");
	if (!file) {
		set_error("cannot write %s: %s", temp, strerror(errno));
		return -1;
	}

	fprintf(file, "power=%s\n", st->power);
	fprintf(file, "color=%s\n", st->color);
	fprintf(file, "brightness=%d\n", st->brightness);
	fprintf(file, "effect=%s\n", st->effect);
	fprintf(file, "zone=%s\n", st->zone);
	fchmod(fileno(file), 0664);
	if (fclose(file) != 0) {
		set_error("cannot close %s: %s", temp, strerror(errno));
		unlink(temp);
		return -1;
	}

	if (rename(temp, path) < 0) {
		set_error("cannot install %s: %s", path, strerror(errno));
		unlink(temp);
		return -1;
	}

	return 0;
}

static void save_state_auto(const struct status *st)
{
	char path[1024];

	if (user_state_path(path, sizeof(path)) == 0)
		state_save_to(path, st);

	/*
	 * The system state file is used by the boot and resume services.
	 * It is group-writable when the package is installed.  Failure is
	 * harmless for users who are not in the alg-rgb group yet.
	 */
	if (access(STATE_DIR, W_OK) == 0)
		state_save_to(STATE_SYSTEM, st);
}

/* -------------------------------------------------------------------------- */
/* Software animation                                                         */
/* -------------------------------------------------------------------------- */

/*
 * The AL15G-52 acknowledges the vendor animation packets but does not move
 * the LEDs.  Motion is therefore rendered in userspace as a stream of normal
 * colour frames over the verified _DSM 0x67 path.  A detached worker owns the
 * frame loop so the dashboard and GNOME extension can start an effect and
 * return immediately.  The saved state stays on the static base colour.
 */

static int animation_state_path(char *path, size_t cap)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	int length;

	if (runtime && *runtime)
		length = snprintf(path, cap, "%s/%s", runtime,
				  ANIMATION_LOCK_NAME);
	else
		length = snprintf(path, cap, "/tmp/%s-%u", ANIMATION_LOCK_NAME,
				  (unsigned int)getuid());

	if (length < 0 || (size_t)length >= cap) {
		set_error("animation state path is too long");
		return -1;
	}

	return 0;
}

static int animation_open(bool create)
{
	char path[512];
	int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW;

	if (animation_state_path(path, sizeof(path)) < 0)
		return -1;

	if (create)
		flags |= O_CREAT;

	return open(path, flags, 0600);
}

static int read_animation_state(int fd, struct animation_state *state)
{
	char buffer[256];
	ssize_t count;
	long pid;

	count = pread(fd, buffer, sizeof(buffer) - 1, 0);
	if (count <= 0)
		return -1;
	buffer[count] = '\0';

	if (sscanf(buffer, "%ld %31s %15s %x %d", &pid, state->effect,
		   state->color, &state->zone, &state->delay_ms) != 5)
		return -1;
	if (pid <= 1)
		return -1;

	state->pid = (pid_t)pid;
	return 0;
}

static int animation_state_active(struct animation_state *state)
{
	int fd;
	int active = 0;

	fd = animation_open(false);
	if (fd < 0)
		return 0;

	if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
		flock(fd, LOCK_UN);
		close(fd);
		return 0;
	}

	if (errno == EWOULDBLOCK || errno == EAGAIN)
		active = read_animation_state(fd, state) == 0;

	close(fd);
	return active;
}

/*
 * Make status output describe the animation base colour and effect rather
 * than whichever frame the driver happened to apply last.  Persistence still
 * uses the raw driver status, so boot restores a static colour.
 */
static void animation_apply_status(struct status *st)
{
	struct animation_state state;
	unsigned int r, g, b;

	if (!animation_state_active(&state))
		return;

	snprintf(st->effect, sizeof(st->effect), "%s", state.effect);
	if (parse_color(state.color, &r, &g, &b) == 0) {
		st->r = r;
		st->g = g;
		st->b = b;
		snprintf(st->color, sizeof(st->color), "#%02x%02x%02x",
			 r, g, b);
	}
	if (state.zone == 0)
		snprintf(st->zone, sizeof(st->zone), "all");
	else
		snprintf(st->zone, sizeof(st->zone), "0x%02x", state.zone);
	snprintf(st->power, sizeof(st->power), "on");
}

static int stop_animation_daemon(bool report)
{
	struct animation_state state;
	struct timespec wait_time = {
		.tv_sec = 0,
		.tv_nsec = 25000000L,
	};
	int fd;
	int attempt;

	fd = animation_open(false);
	if (fd < 0) {
		if (errno == ENOENT) {
			if (report)
				printf("No background animation is running.\n");
			return 0;
		}
		set_error("cannot open animation state: %s", strerror(errno));
		return -1;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
		ftruncate(fd, 0);
		close(fd);
		if (report)
			printf("No background animation is running.\n");
		return 0;
	}

	if (errno != EWOULDBLOCK && errno != EAGAIN) {
		set_error("cannot lock animation state: %s", strerror(errno));
		close(fd);
		return -1;
	}

	if (read_animation_state(fd, &state) < 0) {
		set_error("invalid background animation state");
		close(fd);
		return -1;
	}

	if (kill(state.pid, SIGTERM) < 0 && errno != ESRCH) {
		set_error("cannot stop animation process %ld: %s",
			  (long)state.pid, strerror(errno));
		close(fd);
		return -1;
	}

	for (attempt = 0; attempt < 80; attempt++) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
			ftruncate(fd, 0);
			close(fd);
			if (report)
				printf("Stopped background animation: %s\n",
				       state.effect);
			return 0;
		}

		if (errno != EWOULDBLOCK && errno != EAGAIN)
			break;

		nanosleep(&wait_time, NULL);
	}

	set_error("background animation did not stop within %d ms",
		  ANIMATION_WAIT_MS);
	close(fd);
	return -1;
}

static void handle_animation_signal(int signal_number)
{
	(void)signal_number;
	animation_running = 0;
}

static int animation_write(int fd, const char *command)
{
	size_t length = strlen(command);
	ssize_t written;

	do {
		written = write(fd, command, length);
	} while (written < 0 && errno == EINTR && animation_running);

	if (written < 0) {
		set_error("animation write failed: %s", strerror(errno));
		return -1;
	}

	if ((size_t)written != length) {
		set_error("short animation write (%zd of %zu bytes)", written,
			  length);
		return -1;
	}

	return 0;
}

static void animation_sleep(int delay_ms)
{
	struct timespec remaining = {
		.tv_sec = delay_ms / 1000,
		.tv_nsec = (delay_ms % 1000) * 1000000L,
	};

	while (animation_running &&
	       nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
		;
}

static double animation_hue(unsigned int r, unsigned int g, unsigned int b)
{
	double red = r / 255.0;
	double green = g / 255.0;
	double blue = b / 255.0;
	double maximum = red;
	double minimum = red;
	double delta;
	double hue = 0.0;

	if (green > maximum)
		maximum = green;
	if (blue > maximum)
		maximum = blue;
	if (green < minimum)
		minimum = green;
	if (blue < minimum)
		minimum = blue;

	delta = maximum - minimum;
	if (delta <= 0.0)
		return 0.0;

	if (maximum == red)
		hue = 60.0 * fmod((green - blue) / delta, 6.0);
	else if (maximum == green)
		hue = 60.0 * (((blue - red) / delta) + 2.0);
	else
		hue = 60.0 * (((red - green) / delta) + 4.0);

	if (hue < 0.0)
		hue += 360.0;

	return hue;
}

static void animation_hsv(double hue, double value, unsigned int *r,
			  unsigned int *g, unsigned int *b)
{
	double section = fmod(hue, 360.0) / 60.0;
	double chroma = value;
	double x = chroma * (1.0 - fabs(fmod(section, 2.0) - 1.0));
	double red = 0.0;
	double green = 0.0;
	double blue = 0.0;

	if (section < 1.0) {
		red = chroma;
		green = x;
	} else if (section < 2.0) {
		red = x;
		green = chroma;
	} else if (section < 3.0) {
		green = chroma;
		blue = x;
	} else if (section < 4.0) {
		green = x;
		blue = chroma;
	} else if (section < 5.0) {
		red = x;
		blue = chroma;
	} else {
		red = chroma;
		blue = x;
	}

	*r = (unsigned int)lround(red * 255.0);
	*g = (unsigned int)lround(green * 255.0);
	*b = (unsigned int)lround(blue * 255.0);
}

static void animation_scale_base(const struct status *base, double value,
				 unsigned int *r, unsigned int *g,
				 unsigned int *b)
{
	*r = (unsigned int)lround(base->r * value);
	*g = (unsigned int)lround(base->g * value);
	*b = (unsigned int)lround(base->b * value);
}

static int s_use_frame = 1;

static int animation_send_frame(int fd, unsigned int r, unsigned int g, unsigned int b, unsigned int zone)
{
	char command[80];
	int length;

	if (s_use_frame) {
		length = snprintf(command, sizeof(command),
				  "frame %u %u %u 0x%02x\n", r, g, b, zone);
		if (length > 0 && (size_t)length < sizeof(command)) {
			if (animation_write(fd, command) == 0)
				return 0;
			/* Kernel driver does not support 'frame'. Fall back to 'color'. */
			s_use_frame = 0;
		}
	}

	length = snprintf(command, sizeof(command),
			  "color %u %u %u 0x%02x\n", r, g, b, zone);
	if (length < 0 || (size_t)length >= sizeof(command))
		return -1;

	return animation_write(fd, command);
}

struct anim_zone_spec {
	unsigned int zone;
	double offset;
};

static const struct anim_zone_spec s_anim_zones[] = {
	{ ALG_ZONE_MAIN,  0.00 },
	{ ALG_ZONE_MID,   0.25 },
	{ ALG_ZONE_RIGHT, 0.50 },
	{ ALG_ZONE_BAR,   0.25 },
	{ ALG_ZONE_LOGO,  0.00 },
};

static void animation_frame_color(const char *effect, const struct status *base,
				  double elapsed, unsigned long frame, double offset,
				  unsigned int *r, unsigned int *g,
				  unsigned int *b)
{
	static const unsigned int palette[8][3] = {
		{ 255, 59, 48 },   { 255, 127, 0 },
		{ 255, 214, 10 },  { 0, 255, 70 },
		{ 0, 255, 200 },   { 0, 127, 200 },
		{ 124, 77, 255 },  { 255, 0, 200 },
	};
	double hue = animation_hue(base->r, base->g, base->b);
	double value;
	unsigned int seed;
	size_t index;

	if (!strcmp(effect, "flash")) {
		if (fmod(elapsed, 0.6) < 0.3) {
			*r = base->r;
			*g = base->g;
			*b = base->b;
		} else {
			*r = 0;
			*g = 0;
			*b = 0;
		}
		return;
	}

	if (!strcmp(effect, "tempo")) {
		double phase = fmod(elapsed, 0.5) / 0.5;

		value = 0.12 + 0.88 * (1.0 - phase) * (1.0 - phase);
		animation_scale_base(base, value, r, g, b);
		return;
	}

	if (!strcmp(effect, "breath")) {
		value = 0.08 + 0.92 *
			(0.5 + 0.5 * sin(2.0 * M_PI * elapsed / 3.0));
		animation_scale_base(base, value, r, g, b);
		return;
	}

	if (!strcmp(effect, "wave")) {
		double t = elapsed - offset;
		value = 0.20 + 0.80 *
			(0.5 + 0.5 * sin(2.0 * M_PI * t / 2.0));
		animation_hsv(hue + t * 60.0, value, r, g, b);
		return;
	}

	if (!strcmp(effect, "cycle")) {
		double t = elapsed - offset * 0.5;
		animation_hsv(hue + t * 45.0, 1.0, r, g, b);
		return;
	}

	if (!strcmp(effect, "dance")) {
		double t = elapsed - offset * 0.5;
		value = 0.45 + 0.55 *
			(0.5 + 0.5 * sin(2.0 * M_PI * t / 0.5));
		animation_hsv(hue + 120.0 * floor(t / 0.25), value,
			      r, g, b);
		return;
	}

	if (!strcmp(effect, "random")) {
		seed = (unsigned int)(frame * 2654435761u) ^
		       (unsigned int)(elapsed * 1000.0) ^
		       ((unsigned int)(offset * 10000.0));
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		index = seed % ARRAY_SIZE(palette);
		*r = palette[index][0];
		*g = palette[index][1];
		*b = palette[index][2];
		return;
	}

	*r = base->r;
	*g = base->g;
	*b = base->b;
}

static int run_animation(int fd, const char *effect, const struct status *base,
			 unsigned int target_zone, bool animate_all, int delay_ms)
{
	struct timespec start;
	struct timespec now;
	unsigned long frame = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);

	while (animation_running) {
		double elapsed;
		unsigned int r, g, b;

		if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
			elapsed = (double)(now.tv_sec - start.tv_sec) +
				  (double)(now.tv_nsec - start.tv_nsec) / 1e9;
		else
			elapsed = (double)frame * delay_ms / 1000.0;

		if (animate_all) {
			size_t i;
			for (i = 0; i < ARRAY_SIZE(s_anim_zones) && animation_running; i++) {
				animation_frame_color(effect, base, elapsed, frame,
						      s_anim_zones[i].offset, &r, &g, &b);
				if (animation_send_frame(fd, r, g, b, s_anim_zones[i].zone) < 0)
					return 1;
			}
		} else {
			animation_frame_color(effect, base, elapsed, frame,
					      0.0, &r, &g, &b);
			if (animation_send_frame(fd, r, g, b, target_zone) < 0)
				return 1;
		}

		frame++;
		animation_sleep(delay_ms);
	}

	return 0;
}

static int start_animation_daemon(const char *effect, const struct status *base,
				  int delay_ms)
{
	char ready = '1';
	int ready_pipe[2];
	int state_fd = -1;
	int device_fd = -1;
	int null_fd;
	int result;
	bool animate_all = false;
	unsigned int zone = ALG_ZONE_MAIN;
	pid_t child;
	struct pollfd poll_fd;
	int poll_result;

	if (!strcasecmp(base->zone, "all") || !strcmp(base->zone, "0") || !strcmp(base->zone, "0x00")) {
		animate_all = true;
	} else if (parse_zone(base->zone, &zone) < 0) {
		animate_all = true;
	} else if (zone == 0) {
		animate_all = true;
	}

	if (stop_animation_daemon(false) < 0)
		return -1;

	if (pipe2(ready_pipe, O_CLOEXEC) < 0) {
		set_error("cannot create animation pipe: %s", strerror(errno));
		return -1;
	}

	child = fork();
	if (child < 0) {
		set_error("cannot start animation process: %s", strerror(errno));
		close(ready_pipe[0]);
		close(ready_pipe[1]);
		return -1;
	}

	if (child == 0) {
		struct sigaction action;

		close(ready_pipe[0]);
		animation_running = 1;

		if (setsid() < 0)
			goto child_fail;

		signal(SIGHUP, SIG_IGN);

		memset(&action, 0, sizeof(action));
		action.sa_handler = handle_animation_signal;
		sigemptyset(&action.sa_mask);
		sigaction(SIGINT, &action, NULL);
		sigaction(SIGTERM, &action, NULL);

		state_fd = animation_open(true);
		if (state_fd < 0 ||
		    flock(state_fd, LOCK_EX | LOCK_NB) < 0 ||
		    ftruncate(state_fd, 0) < 0 ||
		    dprintf(state_fd, "%ld %s %s 0x%02x %d\n",
			    (long)getpid(), effect, base->color, animate_all ? 0 : zone,
			    delay_ms) < 0)
			goto child_fail;

		device_fd = open(g_device, O_WRONLY);
		if (device_fd < 0)
			goto child_fail;

		if (animate_all) {
			size_t i;
			for (i = 0; i < ARRAY_SIZE(s_anim_zones); i++) {
				if (animation_send_frame(device_fd, base->r, base->g, base->b, s_anim_zones[i].zone) < 0)
					goto child_fail;
			}
		} else {
			if (animation_send_frame(device_fd, base->r, base->g, base->b, zone) < 0)
				goto child_fail;
		}

		if (write(ready_pipe[1], "0", 1) != 1)
			_exit(1);
		close(ready_pipe[1]);

		null_fd = open("/dev/null", O_RDWR);
		if (null_fd >= 0) {
			dup2(null_fd, STDIN_FILENO);
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			if (null_fd > STDERR_FILENO)
				close(null_fd);
		}

		result = run_animation(device_fd, effect, base, zone, animate_all, delay_ms);

		if (animate_all) {
			size_t i;
			for (i = 0; i < ARRAY_SIZE(s_anim_zones); i++)
				animation_send_frame(device_fd, base->r, base->g, base->b, s_anim_zones[i].zone);
		} else {
			animation_send_frame(device_fd, base->r, base->g, base->b, zone);
		}

		close(device_fd);
		close(state_fd);
		_exit(result ? 1 : 0);
	}

	close(ready_pipe[1]);
	poll_fd.fd = ready_pipe[0];
	poll_fd.events = POLLIN;
	poll_fd.revents = 0;
	poll_result = poll(&poll_fd, 1, ANIMATION_WAIT_MS);
	if (poll_result <= 0 || read(ready_pipe[0], &ready, 1) != 1 ||
	    ready != '0') {
		if (poll_result == 0)
			set_error("timed out starting %s animation", effect);
		else
			set_error("could not start %s animation", effect);
		close(ready_pipe[0]);
		return -1;
	}

	close(ready_pipe[0]);
	return 0;

child_fail:
	if (device_fd >= 0)
		close(device_fd);
	if (state_fd >= 0)
		close(state_fd);
	write(ready_pipe[1], "1", 1);
	_exit(1);
}

static void state_defaults(struct status *st)
{
	memset(st, 0, sizeof(*st));
	st->ok = true;
	snprintf(st->version, sizeof(st->version), "%s", ALG_RGB_VERSION_STRING);
	snprintf(st->power, sizeof(st->power), "on");
	snprintf(st->color, sizeof(st->color), "#ffffff");
	st->brightness = 2;
	snprintf(st->effect, sizeof(st->effect), "none");
	snprintf(st->zone, sizeof(st->zone), "0xf0");
	st->r = 255;
	st->g = 255;
	st->b = 255;
}

static int state_load_file(const char *path, struct status *st, bool allow_missing)
{
	char line[256];
	FILE *file;
	bool seen_power = false;
	bool seen_color = false;
	bool seen_brightness = false;
	bool seen_effect = false;
	bool seen_zone = false;

	state_defaults(st);

	file = fopen(path, "r");
	if (!file) {
		if (allow_missing && errno == ENOENT)
			return 0;
		set_error("cannot read %s: %s", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), file)) {
		char *eq;
		char *value;

		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#')
			continue;

		eq = strchr(line, '=');
		if (!eq) {
			set_error("malformed state line in %s: %s", path, line);
			fclose(file);
			return -1;
		}
		*eq = '\0';
		value = eq + 1;

		if (!strcmp(line, "power")) {
			if (strcmp(value, "on") && strcmp(value, "off")) {
				set_error("invalid power value in %s", path);
				fclose(file);
				return -1;
			}
			snprintf(st->power, sizeof(st->power), "%s", value);
			seen_power = true;
		} else if (!strcmp(line, "color")) {
			unsigned int r, g, b;

			if (parse_color(value, &r, &g, &b) < 0) {
				set_error("invalid color value in %s: %s", path,
					  value);
				fclose(file);
				return -1;
			}
			snprintf(st->color, sizeof(st->color), "#%02x%02x%02x",
				 r, g, b);
			st->r = r;
			st->g = g;
			st->b = b;
			seen_color = true;
		} else if (!strcmp(line, "brightness")) {
			int level;

			if (parse_brightness(value, &level) < 0) {
				set_error("invalid brightness value in %s", path);
				fclose(file);
				return -1;
			}
			st->brightness = level;
			seen_brightness = true;
		} else if (!strcmp(line, "effect")) {
			if (effect_exists(value) < 0) {
				set_error("invalid effect value in %s: %s", path,
					  value);
				fclose(file);
				return -1;
			}
			canonical_effect(value, st->effect, sizeof(st->effect));
			seen_effect = true;
		} else if (!strcmp(line, "zone")) {
			unsigned int zone;

			if (parse_zone(value, &zone) < 0) {
				set_error("invalid zone value in %s: %s", path,
					  value);
				fclose(file);
				return -1;
			}
			snprintf(st->zone, sizeof(st->zone), "0x%02x", zone);
			seen_zone = true;
		}
	}

	if (ferror(file)) {
		set_error("error reading %s", path);
		fclose(file);
		return -1;
	}
	fclose(file);

	if (!seen_power || !seen_color || !seen_brightness || !seen_effect ||
	    !seen_zone) {
		set_error("incomplete state file %s", path);
		return -1;
	}

	return 0;
}

static int finish_status(struct options *opts, struct status *st, bool save)
{
	if (save)
		save_state_auto(st);

	if (opts->json) {
		struct status shown = *st;

		animation_apply_status(&shown);
		status_json(stdout, &shown);
	} else {
		struct status shown = *st;

		animation_apply_status(&shown);
		status_human(&shown, false);
	}

	return 0;
}

static int finish_command(struct options *opts, bool save)
{
	struct status st;

	if (status_load(&st) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	return finish_status(opts, &st, save);
}

static int do_color(struct options *opts, const char *color_text,
		    const char *brightness_text)
{
	unsigned int r, g, b;
	unsigned int zone = 0;
	int brightness = -1;
	bool zone_all = false;

	if (parse_color(color_text, &r, &g, &b) < 0) {
		print_error(opts->json, "unknown colour '%s'", color_text);
		return 1;
	}

	if (brightness_text) {
		if (parse_brightness(brightness_text, &brightness) < 0) {
			print_error(opts->json,
				    "brightness must be 0..%d",
				    ALG_BRIGHTNESS_MAX);
			return 1;
		}
	} else if (opts->have_brightness) {
		brightness = opts->brightness;
	}

	if (opts->zone) {
		if (!strcasecmp(opts->zone, "all")) {
			zone_all = true;
		} else if (parse_zone(opts->zone, &zone) < 0) {
			print_error(opts->json, "unknown zone '%s'",
				    opts->zone);
			return 1;
		}
	}

	if (stop_animation_daemon(false) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (zone_all) {
		static const unsigned int all_zones[] = {
			ALG_ZONE_MAIN, ALG_ZONE_MID, ALG_ZONE_RIGHT,
			ALG_ZONE_BAR, ALG_ZONE_LOGO
		};
		size_t i;

		for (i = 0; i < ARRAY_SIZE(all_zones); i++) {
			if (write_command("color %u %u %u 0x%02x", r, g, b,
					  all_zones[i]) < 0) {
				print_error(opts->json, "%s", g_error);
				return 1;
			}
		}
		/* Leave the default zone at the main keyboard zone. */
		if (write_command("zone 0x%02x", ALG_ZONE_MAIN) < 0) {
			print_error(opts->json, "%s", g_error);
			return 1;
		}
	} else if (opts->zone) {
		if (write_command("color %u %u %u 0x%02x", r, g, b, zone) < 0) {
			print_error(opts->json, "%s", g_error);
			return 1;
		}
	} else {
		if (write_command("color %u %u %u", r, g, b) < 0) {
			print_error(opts->json, "%s", g_error);
			return 1;
		}
	}

	if (brightness >= 0) {
		if (write_command("brightness %d", brightness) < 0) {
			print_error(opts->json, "%s", g_error);
			return 1;
		}
	}

	return finish_command(opts, true);
}

static int do_brightness(struct options *opts, const char *text)
{
	int level;

	if (parse_brightness(text, &level) < 0) {
		print_error(opts->json, "brightness must be 0..%d",
			    ALG_BRIGHTNESS_MAX);
		return 1;
	}

	if (stop_animation_daemon(false) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (write_command("brightness %d", level) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	return finish_command(opts, true);
}

static int do_effect(struct options *opts, const char *text)
{
	struct status base;
	char effect[32];

	if (effect_exists(text) < 0) {
		print_error(opts->json, "unknown effect '%s'", text);
		return 1;
	}

	canonical_effect(text, effect, sizeof(effect));

	if (!strcmp(effect, "none")) {
		if (stop_animation_daemon(false) < 0) {
			print_error(opts->json, "%s", g_error);
			return 1;
		}

		if (write_command("effect none") < 0) {
			print_error(opts->json, "%s", g_error);
			return 1;
		}

		return finish_command(opts, true);
	}

	/*
	 * Software animation frames are generated by a detached worker.  The
	 * driver keeps the base colour in its state, so status and persistence
	 * stay meaningful while the worker streams frames.
	 */
	if (status_load(&base) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	snprintf(base.effect, sizeof(base.effect), "none");
	snprintf(base.power, sizeof(base.power), "on");
	if (base.brightness == 0)
		base.brightness = 3;

	if (opts->zone) {
		if (!strcasecmp(opts->zone, "all")) {
			snprintf(base.zone, sizeof(base.zone), "all");
		} else {
			unsigned int z;
			if (parse_zone(opts->zone, &z) < 0) {
				print_error(opts->json, "unknown zone '%s'", opts->zone);
				return 1;
			}
			snprintf(base.zone, sizeof(base.zone), "0x%02x", z);
		}
	} else {
		snprintf(base.zone, sizeof(base.zone), "all");
	}

	if (start_animation_daemon(effect, &base, ANIMATION_DELAY_DEFAULT) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	return finish_status(opts, &base, true);
}

static int do_stop(struct options *opts)
{
	if (stop_animation_daemon(true) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (opts->json)
		fputs("{\"ok\":true,\"effect\":\"none\"}\n", stdout);

	return 0;
}

static int do_power(struct options *opts, bool on)
{
	if (stop_animation_daemon(false) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (write_command(on ? "on" : "off") < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	return finish_command(opts, true);
}

static int do_zone(struct options *opts, const char *text)
{
	unsigned int zone;

	if (parse_zone(text, &zone) < 0) {
		print_error(opts->json, "unknown zone '%s'", text);
		return 1;
	}

	if (stop_animation_daemon(false) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (write_command("zone 0x%02x", zone) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	return finish_command(opts, true);
}

static int do_status(struct options *opts)
{
	struct status st;

	if (status_load(&st) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	animation_apply_status(&st);

	if (opts->json)
		status_json(stdout, &st);
	else
		status_human(&st, true);

	return 0;
}

static int collect_hid_devices(char names[][HID_NAME_LEN], int max,
			       bool *perkey)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	*perkey = false;
	dir = opendir("/sys/bus/hid/devices");
	if (!dir)
		return 0;

	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		if (strcasestr(entry->d_name, "048D") &&
		    strcasestr(entry->d_name, "8910"))
			*perkey = true;
		if (count < max) {
			snprintf(names[count], HID_NAME_LEN, "%.127s",
				 entry->d_name);
			count++;
		}
	}
	closedir(dir);

	return count;
}

static bool scan_usb_for_perkey(void)
{
	glob_t globbuf;
	size_t i;
	bool found = false;

	if (glob("/sys/bus/usb/devices/*", 0, NULL, &globbuf) != 0)
		return false;

	for (i = 0; i < globbuf.gl_pathc; i++) {
		char vendor_path[512];
		char product_path[512];
		char vendor[16] = "";
		char product[16] = "";

		snprintf(vendor_path, sizeof(vendor_path), "%s/idVendor",
			 globbuf.gl_pathv[i]);
		snprintf(product_path, sizeof(product_path), "%s/idProduct",
			 globbuf.gl_pathv[i]);

		if (read_text_file(vendor_path, vendor, sizeof(vendor)) < 0)
			continue;
		if (read_text_file(product_path, product, sizeof(product)) < 0)
			continue;

		if (!strcasecmp(vendor, "048d") &&
		    !strcasecmp(product, "8910")) {
			found = true;
			break;
		}
	}

	globfree(&globbuf);
	return found;
}

static void print_capabilities_json(FILE *out)
{
	char caps[2048] = "";
	char *save = NULL;
	char *line;
	bool first = true;

	fputs("[", out);
	if (read_text_file(CAPABILITIES_PATH, caps, sizeof(caps)) == 0) {
		line = strtok_r(caps, "\n", &save);
		while (line) {
			if (!first)
				fputc(',', out);
			json_string(out, line);
			first = false;
			line = strtok_r(NULL, "\n", &save);
		}
	}
	fputs("]", out);
}

static int do_probe(struct options *opts)
{
	char hid_names[MAX_HID_DEVICES][HID_NAME_LEN];
	char acpi_status[32] = "";
	char module_version[64] = "";
	char caps[2048] = "";
	bool hid_perkey = false;
	bool usb_perkey = false;
	bool perkey;
	bool acpi_present;
	bool module_present;
	bool device_present;
	bool device_accessible;
	struct status st;
	bool have_status = false;
	int hid_count;
	int i;

	hid_count = collect_hid_devices(hid_names, MAX_HID_DEVICES, &hid_perkey);
	usb_perkey = scan_usb_for_perkey();
	perkey = hid_perkey || usb_perkey;

	acpi_present = access(ACPI_DEVICE_PATH, F_OK) == 0;
	module_present = access("/sys/module/alg_rgb", F_OK) == 0;
	device_present = access(g_device, F_OK) == 0;
	device_accessible = access(g_device, R_OK | W_OK) == 0;

	if (acpi_present)
		read_text_file(ACPI_DEVICE_PATH "/status", acpi_status,
			       sizeof(acpi_status));
	if (module_present)
		read_text_file(MODULE_VERSION_PATH, module_version,
			       sizeof(module_version));
	read_text_file(CAPABILITIES_PATH, caps, sizeof(caps));

	if (status_load(&st) == 0)
		have_status = true;

	if (opts->json) {
		fputs("{\"ok\":true,", stdout);
		fputs("\"acpi\":{\"device\":\"CLV0001:00\",\"present\":", stdout);
		fputs(acpi_present ? "true" : "false", stdout);
		fputs(",\"status\":", stdout);
		json_string(stdout, acpi_status);
		fputs("},\"module\":{\"present\":", stdout);
		fputs(module_present ? "true" : "false", stdout);
		fputs(",\"version\":", stdout);
		json_string(stdout, module_version);
		fputs("},\"device\":{\"path\":", stdout);
		json_string(stdout, g_device);
		fputs(",\"present\":", stdout);
		fputs(device_present ? "true" : "false", stdout);
		fputs(",\"accessible\":", stdout);
		fputs(device_accessible ? "true" : "false", stdout);
		fputs("},\"hid\":{\"perkey_id\":\"048D:8910\",\"perkey_present\":", stdout);
		fputs(perkey ? "true" : "false", stdout);
		fputs(",\"devices\":[", stdout);
		for (i = 0; i < hid_count; i++) {
			if (i)
				fputc(',', stdout);
			json_string(stdout, hid_names[i]);
		}
		fputs("]},\"perkey\":{\"available\":", stdout);
		fputs(perkey ? "true" : "false", stdout);
		fputs(",\"reason\":", stdout);
		if (perkey)
			json_string(stdout,
				    "HID 048D:8910 is present. No live per-key ACPI protocol is verified, so per-key control is not implemented yet.");
		else
			json_string(stdout,
				    "HID 048D:8910 (ITE 829x per-key controller) is not enumerated. Only the CLV0001 zone-RGB path is exposed.");
		fputs("},\"capabilities\":", stdout);
		print_capabilities_json(stdout);
		if (have_status) {
			fputs(",\"status\":{", stdout);
			status_fields_json(stdout, &st);
			fputs("}", stdout);
		}
		fputs("}\n", stdout);
		return 0;
	}

	printf("ACPI CLV0001:   %s%s%s\n", acpi_present ? "present" : "missing",
	       acpi_status[0] ? ", status " : "",
	       acpi_status[0] ? acpi_status : "");
	printf("Kernel module:  %s%s%s\n", module_present ? "loaded" : "not loaded",
	       module_version[0] ? ", version " : "",
	       module_version[0] ? module_version : "");
	printf("Device node:    %s (%s)\n", device_present ? g_device : "missing",
	       device_accessible ? "read/write" : "not accessible");
	printf("Per-key HID:    %s\n", perkey ? "048D:8910 present" :
	       "048D:8910 not present");
	printf("HID devices:    ");
	if (hid_count == 0) {
		printf("(none)\n");
	} else {
		for (i = 0; i < hid_count; i++)
			printf("%s%s", i ? ", " : "", hid_names[i]);
		printf("\n");
	}
	printf("Per-key result: %s\n", perkey ?
	       "HID present, but no verified per-key protocol is implemented" :
	       "unavailable - zone RGB is the only exposed lighting path");
	if (have_status) {
		printf("Current state:  ");
		status_human(&st, false);
	}
	if (caps[0]) {
		printf("\nVerified capabilities:\n%s\n", caps);
	}
	if (!have_status) {
		printf("\nNote: %s\n", g_error);
	}

	return 0;
}

static int do_list(struct options *opts)
{
	size_t i;

	if (opts->json) {
		fputs("{\"ok\":true,\"version\":", stdout);
		json_string(stdout, ALG_RGB_VERSION_STRING);
		fputs(",\"colors\":[", stdout);
		for (i = 0; i < ARRAY_SIZE(named_colors); i++) {
			if (i)
				fputc(',', stdout);
			fputs("{\"name\":", stdout);
			json_string(stdout, named_colors[i].name);
			fprintf(stdout, ",\"hex\":\"#%02x%02x%02x\"}",
				named_colors[i].r, named_colors[i].g,
				named_colors[i].b);
		}
		fputs("],\"effects\":[", stdout);
		for (i = 0; i < ARRAY_SIZE(named_effects); i++) {
			if (i)
				fputc(',', stdout);
			fputs("{\"name\":", stdout);
			json_string(stdout, named_effects[i].name);
			fputs(",\"description\":", stdout);
			json_string(stdout, named_effects[i].description);
			fputc('}', stdout);
		}
		fputs("],\"zones\":[", stdout);
		for (i = 0; i < ARRAY_SIZE(named_zones); i++) {
			if (i)
				fputc(',', stdout);
			fputs("{\"name\":", stdout);
			json_string(stdout, named_zones[i].name);
			fprintf(stdout, ",\"id\":\"0x%02x\",\"description\":",
				named_zones[i].id);
			json_string(stdout, named_zones[i].description);
			fputc('}', stdout);
		}
		fputs("],\"brightness_levels\":[0,1,2,3,4]}\n", stdout);
		return 0;
	}

	printf("Colours:\n");
	for (i = 0; i < ARRAY_SIZE(named_colors); i++)
		printf("  %-14s #%02X%02X%02X\n", named_colors[i].name,
		       named_colors[i].r, named_colors[i].g,
		       named_colors[i].b);

	printf("\nEffects:\n");
	for (i = 0; i < ARRAY_SIZE(named_effects); i++)
		printf("  %-8s %s\n", named_effects[i].name,
		       named_effects[i].description);

	printf("\nZones:\n");
	for (i = 0; i < ARRAY_SIZE(named_zones); i++)
		printf("  %-6s 0x%02X  %s\n", named_zones[i].name,
		       named_zones[i].id, named_zones[i].description);

	printf("\nBrightness: 0 (off) .. %d (maximum)\n", ALG_BRIGHTNESS_MAX);
	return 0;
}

static int do_save(struct options *opts)
{
	char path[1024];
	struct status st;

	if (status_load(&st) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (opts->file) {
		snprintf(path, sizeof(path), "%s", opts->file);
	} else if (opts->system) {
		snprintf(path, sizeof(path), "%s", STATE_SYSTEM);
	} else if (user_state_path(path, sizeof(path)) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (state_save_to(path, &st) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (opts->json) {
		fputs("{\"ok\":true,\"path\":", stdout);
		json_string(stdout, path);
		fputs(",\"status\":{", stdout);
		status_fields_json(stdout, &st);
		fputs("}}\n", stdout);
	} else {
		printf("Saved keyboard state to %s\n", path);
	}

	return 0;
}

static int do_restore(struct options *opts)
{
	char path[1024];
	struct status st;
	bool allow_missing = false;
	int ret;

	if (stop_animation_daemon(false) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (opts->file) {
		snprintf(path, sizeof(path), "%s", opts->file);
	} else if (opts->system) {
		snprintf(path, sizeof(path), "%s", STATE_SYSTEM);
		allow_missing = true;
	} else if (user_state_path(path, sizeof(path)) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (state_load_file(path, &st, allow_missing) < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (!strcmp(st.power, "off")) {
		ret = write_command("off");
	} else {
		if (!strcmp(st.effect, "none")) {
			ret = write_command("color %u %u %u %s", st.r, st.g,
					    st.b, st.zone);
		} else {
			ret = write_command("effect %s", st.effect);
		}

		if (ret == 0)
			ret = write_command("brightness %d", st.brightness);
	}

	if (ret < 0) {
		print_error(opts->json, "%s", g_error);
		return 1;
	}

	if (opts->json) {
		struct status current;

		if (status_load(&current) < 0) {
			print_error(true, "%s", g_error);
			return 1;
		}
		fputs("{\"ok\":true,\"path\":", stdout);
		json_string(stdout, path);
		fputs(",\"status\":{", stdout);
		status_fields_json(stdout, &current);
		fputs("}}\n", stdout);
	} else {
		printf("Restored keyboard state from %s\n", path);
	}

	return 0;
}

static void usage(FILE *out, const char *prog)
{
	fprintf(out, "alg-rgb %s - Acer ALG CLV0001 RGB keyboard control\n\n",
		ALG_RGB_VERSION_STRING);
	fprintf(out, "Usage:\n");
	fprintf(out, "  %s color <name|#RRGGBB> [brightness] [--zone ZONE]\n", prog);
	fprintf(out, "  %s <name|#RRGGBB> [brightness]        (short form)\n", prog);
	fprintf(out, "  %s brightness <0-4>\n", prog);
	fprintf(out, "  %s effect <name|none|static>\n", prog);
	fprintf(out, "  %s stop\n", prog);
	fprintf(out, "  %s off | on\n", prog);
	fprintf(out, "  %s zone <name>\n", prog);
	fprintf(out, "  %s status\n", prog);
	fprintf(out, "  %s probe\n", prog);
	fprintf(out, "  %s save [--system|--file PATH]\n", prog);
	fprintf(out, "  %s restore [--system|--file PATH]\n", prog);
	fprintf(out, "  %s list\n\n", prog);

	fprintf(out, "Global options:\n");
	fprintf(out, "  --json              machine-readable output\n");
	fprintf(out, "  --device PATH       device node (default %s)\n", DEFAULT_DEVICE);
	fprintf(out, "  --zone ZONE         main|mid|right|bar|logo|all|0xF0..\n");
	fprintf(out, "  --brightness N      brightness for a colour in one call\n");
	fprintf(out, "  --system            use the system state file\n");
	fprintf(out, "  --file PATH         use a specific state file\n");
	fprintf(out, "  -h, --help          show this help\n");
	fprintf(out, "  --version           show version\n\n");

	fprintf(out, "Colours: ");
	for (size_t i = 0; i < ARRAY_SIZE(named_colors); i++)
		fprintf(out, "%s%s", i ? ", " : "", named_colors[i].name);
	fprintf(out, "\nEffects: ");
	for (size_t i = 0; i < ARRAY_SIZE(named_effects); i++)
		fprintf(out, "%s%s", i ? ", " : "", named_effects[i].name);
	fprintf(out, "\nZones:   main, mid, right, bar, logo, all\n");
	fprintf(out, "Motion:  effects run as software-rendered frames;\n");
	fprintf(out, "         stop returns to the current static colour.\n\n");

	fprintf(out, "Examples:\n");
	fprintf(out, "  %s cyan 3\n", prog);
	fprintf(out, "  %s color '#ff6600' 4 --zone main\n", prog);
	fprintf(out, "  %s effect wave\n", prog);
	fprintf(out, "  %s stop\n", prog);
	fprintf(out, "  %s off\n", prog);
	fprintf(out, "  %s status --json\n", prog);
	fprintf(out, "  %s probe --json\n", prog);
}

static int dispatch(struct options *opts, int argc, char **argv)
{
	const char *command;

	if (argc == 0) {
		usage(stderr, "alg-rgb");
		return 1;
	}

	command = argv[0];

	if (!strcmp(command, "help")) {
		usage(stdout, "alg-rgb");
		return 0;
	}

	if (!strcmp(command, "status"))
		return do_status(opts);

	if (!strcmp(command, "list"))
		return do_list(opts);

	if (!strcmp(command, "probe"))
		return do_probe(opts);

	if (!strcmp(command, "stop"))
		return do_stop(opts);

	if (!strcmp(command, "save"))
		return do_save(opts);

	if (!strcmp(command, "restore"))
		return do_restore(opts);

	if (!strcmp(command, "off"))
		return do_power(opts, false);

	if (!strcmp(command, "on"))
		return do_power(opts, true);

	if (!strcmp(command, "brightness")) {
		if (argc < 2) {
			print_error(opts->json, "brightness requires a value");
			return 1;
		}
		return do_brightness(opts, argv[1]);
	}

	if (!strcmp(command, "effect")) {
		if (argc < 2) {
			print_error(opts->json, "effect requires a name");
			return 1;
		}
		return do_effect(opts, argv[1]);
	}

	if (!strcmp(command, "color")) {
		if (argc < 2) {
			print_error(opts->json, "color requires a value");
			return 1;
		}
		return do_color(opts, argv[1], argc >= 3 ? argv[2] : NULL);
	}

	if (!strcmp(command, "zone")) {
		if (argc < 2) {
			print_error(opts->json, "zone requires a name");
			return 1;
		}
		if (argc == 2)
			return do_zone(opts, argv[1]);

		/* zone <name> <color> [brightness] */
		opts->zone = argv[1];
		return do_color(opts, argv[2], argc >= 4 ? argv[3] : NULL);
	}

	/* Backwards-compatible short form: alg-rgb red [brightness] */
	{
		unsigned int r, g, b;

		if (parse_color(command, &r, &g, &b) == 0)
			return do_color(opts, command,
					argc >= 2 ? argv[1] : NULL);
	}

	print_error(opts->json, "unknown command '%s' (try --help)",
		    command);
	return 1;
}

int main(int argc, char **argv)
{
	struct options opts;
	char *args[64];
	int arg_count = 0;
	int i;

	memset(&opts, 0, sizeof(opts));

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "--json")) {
			opts.json = true;
		} else if (!strcmp(arg, "--device")) {
			if (++i >= argc) {
				print_error(false, "--device requires a path");
				return 1;
			}
			opts.device = argv[i];
		} else if (!strcmp(arg, "--zone")) {
			if (++i >= argc) {
				print_error(false, "--zone requires a name");
				return 1;
			}
			opts.zone = argv[i];
		} else if (!strcmp(arg, "--brightness")) {
			if (++i >= argc ||
			    parse_brightness(argv[i], &opts.brightness) < 0) {
				print_error(false,
					    "--brightness must be 0..%d",
					    ALG_BRIGHTNESS_MAX);
				return 1;
			}
			opts.have_brightness = true;
		} else if (!strcmp(arg, "--system")) {
			opts.system = true;
		} else if (!strcmp(arg, "--file")) {
			if (++i >= argc) {
				print_error(false, "--file requires a path");
				return 1;
			}
			opts.file = argv[i];
		} else if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
			usage(stdout, "alg-rgb");
			return 0;
		} else if (!strcmp(arg, "--version")) {
			if (opts.json) {
				printf("{\"ok\":true,\"version\":\"%s\"}\n",
				       ALG_RGB_VERSION_STRING);
			} else {
				printf("alg-rgb %s\n", ALG_RGB_VERSION_STRING);
			}
			return 0;
		} else if (arg[0] == '-' && arg[1] != '\0') {
			print_error(false, "unknown option '%s'", arg);
			return 1;
		} else {
			if (arg_count >= (int)ARRAY_SIZE(args)) {
				print_error(false, "too many arguments");
				return 1;
			}
			args[arg_count++] = argv[i];
		}
	}

	if (opts.device)
		g_device = opts.device;
	if (opts.file && opts.system) {
		print_error(opts.json, "--file and --system are mutually exclusive");
		return 1;
	}

	return dispatch(&opts, arg_count, args);
}
