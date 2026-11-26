// Preloaded into the program of the PAM tests: stands in for the virtual keyboard of workaround=input, so that
// no key is ever pressed on the computer that runs the tests. Without PAM_TEST_UINPUT /dev/uinput can not be
// used at all.
//   PAM_TEST_UINPUT=ok|fail        the device can be used, or it can not be created
//   PAM_TEST_UINPUT_LOG=<file>     a line is added for every press of Enter
//   PAM_TEST_UINPUT_ANSWER=<file>  is created by a press of Enter (the file PAM_TEST_HOLD_PROMPT waits for)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libevdev/libevdev-uinput.h>

int euidaccess(const char* path, int mode) {
	if (strcmp(path, "/dev/uinput") == 0) {
		return getenv("PAM_TEST_UINPUT") != NULL ? 0 : -1;
	}
	int (*next)(const char*, int);
	*(void**)(&next) = dlsym(RTLD_NEXT, "euidaccess");
	return next(path, mode);
}

int libevdev_uinput_create_from_device(const struct libevdev* dev, int uinput_fd, struct libevdev_uinput** uinput_dev) {
	(void)dev;
	(void)uinput_fd;
	const char* state = getenv("PAM_TEST_UINPUT");
	if (state == NULL || strcmp(state, "ok") != 0) {
		return -EPERM;
	}
	static char device;
	*uinput_dev = (struct libevdev_uinput*)&device;
	return 0;
}

static void append(const char* variable, const char* line) {
	const char* path = getenv(variable);
	FILE* file = path != NULL ? fopen(path, "a") : NULL;
	if (file != NULL) {
		fputs(line, file);
		fclose(file);
	}
}

int libevdev_uinput_write_event(const struct libevdev_uinput* uinput_dev, unsigned int type, unsigned int code, int value) {
	(void)uinput_dev;
	if (type == EV_KEY && code == KEY_ENTER && value == 1) {
		append("PAM_TEST_UINPUT_LOG", "ENTER\n");
		append("PAM_TEST_UINPUT_ANSWER", "ENTER\n");
	}
	return 0;
}

void libevdev_uinput_destroy(struct libevdev_uinput* uinput_dev) {
	(void)uinput_dev;
}
