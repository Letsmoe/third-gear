// Standalone G923 / H-shifter diagnostic, independent of Unreal.
//
//   cc -O2 -Wall -o wheeltest wheeltest.c
//   ./wheeltest            list input devices with force feedback
//   ./wheeltest monitor    print axes and button presses (find shifter gear buttons, pedal axes)
//   ./wheeltest ffb        play a gentle constant force left, then right (checks force direction + permissions)
//
// Uses the first Logitech (046d) device that has an X axis and force feedback, or the path in $WHEEL_DEVICE.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static int has_ffb_and_x(int fd)
{
	unsigned long ev[NLONGS(EV_CNT)] = {0}, abs[NLONGS(ABS_CNT)] = {0};
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0) return 0;
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0) return 0;
	return TEST_BIT(EV_FF, ev) && TEST_BIT(ABS_X, abs);
}

static int open_wheel(int flags, char *path_out, size_t path_len)
{
	const char *env = getenv("WHEEL_DEVICE");
	if (env) {
		snprintf(path_out, path_len, "%s", env);
		return open(env, flags);
	}
	DIR *dir = opendir("/dev/input");
	if (!dir) return -1;
	struct dirent *entry;
	int found = -1;
	while ((entry = readdir(dir)) && found < 0) {
		if (strncmp(entry->d_name, "event", 5) != 0) continue;
		char path[300];
		snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
		int fd = open(path, flags);
		if (fd < 0) continue;
		struct input_id id;
		if (ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == 0x046d && has_ffb_and_x(fd)) {
			snprintf(path_out, path_len, "%s", path);
			found = fd;
		} else {
			close(fd);
		}
	}
	closedir(dir);
	return found;
}

static void list_devices(void)
{
	DIR *dir = opendir("/dev/input");
	struct dirent *entry;
	while (dir && (entry = readdir(dir))) {
		if (strncmp(entry->d_name, "event", 5) != 0) continue;
		char path[300], name[256] = "?";
		snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			if (errno == EACCES) printf("%-20s (no permission)\n", path);
			continue;
		}
		struct input_id id = {0};
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		ioctl(fd, EVIOCGID, &id);
		printf("%-20s %04x:%04x ffb=%s  %s\n", path, id.vendor, id.product, has_ffb_and_x(fd) ? "yes" : "no ", name);
		close(fd);
	}
	if (dir) closedir(dir);
}

static void describe(int fd, const char *path)
{
	char name[256] = "?";
	struct input_id id = {0};
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	ioctl(fd, EVIOCGID, &id);
	printf("Device: %s  %04x:%04x  %s\n", path, id.vendor, id.product, name);

	unsigned long abs[NLONGS(ABS_CNT)] = {0};
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
	for (int code = 0; code < ABS_CNT; code++) {
		if (!TEST_BIT(code, abs)) continue;
		struct input_absinfo info;
		ioctl(fd, EVIOCGABS(code), &info);
		printf("  axis 0x%02x  min=%d max=%d value=%d fuzz=%d flat=%d\n", code, info.minimum, info.maximum, info.value,
		       info.fuzz, info.flat);
	}

	unsigned long keys[NLONGS(KEY_CNT)] = {0};
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
	int index = 0;
	printf("  buttons (index:code):");
	for (int code = 0; code < KEY_CNT; code++)
		if (TEST_BIT(code, keys)) printf(" %d:0x%x", index++, code);
	printf("\n");

	unsigned long ff[NLONGS(FF_CNT)] = {0};
	ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff);
	int effects = 0;
	ioctl(fd, EVIOCGEFFECTS, &effects);
	printf("  ffb: constant=%d spring=%d damper=%d friction=%d periodic=%d gain=%d autocenter=%d  max effects=%d\n",
	       (int)TEST_BIT(FF_CONSTANT, ff), (int)TEST_BIT(FF_SPRING, ff), (int)TEST_BIT(FF_DAMPER, ff),
	       (int)TEST_BIT(FF_FRICTION, ff), (int)TEST_BIT(FF_PERIODIC, ff), (int)TEST_BIT(FF_GAIN, ff),
	       (int)TEST_BIT(FF_AUTOCENTER, ff), effects);
}

static int button_index(int fd, int code)
{
	unsigned long keys[NLONGS(KEY_CNT)] = {0};
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
	int index = 0;
	for (int c = 0; c < code; c++)
		if (TEST_BIT(c, keys)) index++;
	return index;
}

static void monitor(int fd)
{
	printf("Monitoring. Move the wheel, press each pedal fully, put the shifter in every gear. Ctrl+C to stop.\n");
	struct input_event ev;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_ABS)
			printf("axis   0x%02x = %d\n", ev.code, ev.value);
		else if (ev.type == EV_KEY)
			printf("button index %d (code 0x%x) %s\n", button_index(fd, ev.code), ev.code,
			       ev.value ? "DOWN" : "up");
	}
}

static void send_ff(int fd, unsigned short code, int value)
{
	struct input_event ev = {.type = EV_FF, .code = code, .value = value};
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) perror("write EV_FF");
}

static void ffb_test(int fd)
{
	send_ff(fd, FF_AUTOCENTER, 0);
	send_ff(fd, FF_GAIN, 0xFFFF);

	struct ff_effect effect;
	memset(&effect, 0, sizeof(effect));
	effect.type = FF_CONSTANT;
	effect.id = -1;
	effect.direction = 0x4000;  // the convention the game will use; we check which way it pulls
	effect.replay.length = 0;   // infinite
	if (ioctl(fd, EVIOCSFF, &effect) < 0) {
		perror("EVIOCSFF upload");
		return;
	}
	send_ff(fd, effect.id, 1);  // start

	const short levels[] = {6000, 0, -6000, 0};
	const char *labels[] = {"positive level (+6000)", "zero", "negative level (-6000)", "zero"};
	for (int i = 0; i < 4; i++) {
		effect.u.constant.level = levels[i];
		if (ioctl(fd, EVIOCSFF, &effect) < 0) perror("EVIOCSFF update");
		printf("%s ... note which way the wheel pulls\n", labels[i]);
		sleep(2);
	}
	send_ff(fd, effect.id, 0);
	ioctl(fd, EVIOCRMFF, effect.id);
	printf("Done. Report: does the POSITIVE level pull the wheel LEFT or RIGHT?\n");
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "list";
	if (strcmp(mode, "list") == 0) {
		list_devices();
		return 0;
	}
	char path[300] = "";
	int fd = open_wheel(O_RDWR, path, sizeof(path));
	if (fd < 0) {
		fprintf(stderr, "No Logitech wheel with force feedback found (or no permission). Run './wheeltest list'.\n");
		return 1;
	}
	describe(fd, path);
	if (strcmp(mode, "monitor") == 0) monitor(fd);
	else if (strcmp(mode, "ffb") == 0) ffb_test(fd);
	close(fd);
	return 0;
}
