// Run pam_authenticate() of a PAM configuration with the pam_howdy.so of the
// test and print what the module tells the user and what it returns.
// Usage: pam_test <confdir> <service> <user>
#include <cstdio>
#include <cstdlib>

#include <security/pam_appl.h>

namespace {

int conversation(int count, const struct pam_message** messages, struct pam_response** responses, void* /*data*/) {
	*responses = static_cast<struct pam_response*>(calloc(static_cast<size_t>(count), sizeof(struct pam_response)));
	for (int i = 0; i < count; ++i) {
		std::printf("MSG %d: %s\n", messages[i]->msg_style, messages[i]->msg);
	}
	return PAM_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 4) {
		return 2;
	}
	const struct pam_conv conv = {conversation, nullptr};
	pam_handle_t* handle = nullptr;
	if (pam_start_confdir(argv[2], argv[3], &conv, argv[1], &handle) != PAM_SUCCESS) {
		std::printf("pam_start failed\n");
		return 2;
	}
	const int result = pam_authenticate(handle, 0);
	std::printf("RESULT %d %s\n", result, pam_strerror(handle, result));
	pam_end(handle, result);
	return 0;
}
