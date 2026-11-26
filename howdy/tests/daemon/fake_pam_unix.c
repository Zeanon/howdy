// The module that the pam_howdy.so of the test asks for the password: writes the arguments it gets to the file
// in PAM_TEST_UNIX_ARGS, one per line, and leaves the rest to the real pam_unix.so (REAL_PAM_UNIX).
// With the argument "show_authtok" it is a module of the stack of its own instead: it says whether a password
// is there for it.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <security/pam_ext.h>
#include <security/pam_modules.h>

PAM_EXTERN int pam_sm_authenticate(pam_handle_t* pamh, int flags, int argc, const char** argv) {
	if (argc == 1 && strcmp(argv[0], "show_authtok") == 0) {
		const void* authtok = NULL;
		pam_get_item(pamh, PAM_AUTHTOK, &authtok);
		pam_info(pamh, "AUTHTOK %s", authtok != NULL ? "set" : "unset");
		return PAM_SUCCESS;
	}
	const char* path = getenv("PAM_TEST_UNIX_ARGS");
	if (path != NULL) {
		FILE* file = fopen(path, "w");
		if (file != NULL) {
			for (int i = 0; i < argc; i++) {
				fprintf(file, "%s\n", argv[i]);
			}
			fclose(file);
		}
	}
	// (stays loaded, as the module loads it for every authentication)
	void* handle = dlopen(REAL_PAM_UNIX, RTLD_NOW);
	if (handle == NULL) {
		return PAM_AUTHINFO_UNAVAIL;
	}
	int (*authenticate)(pam_handle_t*, int, int, const char**);
	*(void**)(&authenticate) = dlsym(handle, "pam_sm_authenticate");
	return authenticate != NULL ? authenticate(pamh, flags, argc, argv) : PAM_AUTHINFO_UNAVAIL;
}
