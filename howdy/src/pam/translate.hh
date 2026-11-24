#pragma once

#include <libintl.h>
#include <locale.h>

/**
 * Translate a message with the language of the environment, without touching the process.
 *
 * The module runs inside the program that asks for the authentication (the lock screen, sudo, a
 * login manager). setlocale() and textdomain() change the locale and the text domain of the whole
 * program, which does other things with them at the same time (and the lock screen has a thread of
 * its own for every PAM stack): setlocale() is not safe while other threads use the locale. So the
 * locale of the language is only made current for the thread of the call, for as long as the
 * message is looked up.
 *
 * Returns the message itself if there is no translation.
 */
inline const char* translate(const char* message) {
	static const locale_t locale = [] {
		bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
		// The messages are given to the program as they are, in UTF-8
		bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
		return newlocale(LC_MESSAGES_MASK, "", static_cast<locale_t>(nullptr));
	}();
	if (locale == static_cast<locale_t>(nullptr)) {
		return message;
	}

	const locale_t previous = uselocale(locale);
	const char* translated = dgettext(GETTEXT_PACKAGE, message);
	uselocale(previous);
	return translated;
}
