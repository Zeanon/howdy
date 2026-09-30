#include "fprintd_client.hh"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstring>

namespace {

constexpr const char* FPRINTD_BUS =
    "net.reactivated.Fprint";

constexpr const char* FPRINTD_MANAGER =
    "/net/reactivated/Fprint/Manager";

constexpr const char* FPRINTD_MANAGER_IFACE =
    "net.reactivated.Fprint.Manager";

constexpr const char* FPRINTD_DEVICE_IFACE =
    "net.reactivated.Fprint.Device";

constexpr const char* FPRINTD_ERROR_ALREADY_IN_USE =
    "net.reactivated.Fprint.Error.AlreadyInUse";

constexpr const char* FPRINTD_ERROR_NO_ENROLLED_PRINTS =
    "net.reactivated.Fprint.Error.NoEnrolledPrints";

constexpr const char* FPRINTD_ERROR_CLAIM_DEVICE =
    "net.reactivated.Fprint.Error.ClaimDevice";

constexpr const char* FPRINTD_ERROR_NO_ACTION =
    "net.reactivated.Fprint.Error.NoActionInProgress";

} // namespace{



FprintdAuthenticator::FprintdAuthenticator(
    std::string username)
    : username_(std::move(username))
{
}


FprintdAuthenticator::~FprintdAuthenticator()
{
    cleanup();

    if (bus_) {
        sd_bus_unref(bus_);
        bus_ = nullptr;
    }
}


bool FprintdAuthenticator::connect()
{
    const int r = sd_bus_open_system(&bus_);

    if (r < 0) {
        return false;
    }

    /*
     * Watch the fprintd name itself.
     *
     * If fprintd disappears while we are verifying,
     * authentication must fail rather than waiting forever.
     */
    return sd_bus_match_signal(
               bus_,
               &name_owner_slot_,
               "org.freedesktop.DBus",
               "/org/freedesktop/DBus",
               "org.freedesktop.DBus",
               "NameOwnerChanged",
               fprintd_name_owner_changed,
               this) >= 0;
}


bool FprintdAuthenticator::get_default_device()
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    const int r = sd_bus_call_method(
        bus_,
        FPRINTD_BUS,
        FPRINTD_MANAGER,
        FPRINTD_MANAGER_IFACE,
        "GetDefaultDevice",
        &error,
        &reply,
        "");

    if (r < 0) {
        sd_bus_error_free(&error);
        return false;
    }

    const char* path = nullptr;

    const int read_result =
        sd_bus_message_read(
            reply,
            "o",
            &path);

    if (read_result < 0 || path == nullptr) {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return false;
    }

    device_ = path;

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    return true;
}


bool FprintdAuthenticator::claim()
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    const int r = sd_bus_call_method(
        bus_,
        FPRINTD_BUS,
        device_.c_str(),
        FPRINTD_DEVICE_IFACE,
        "Claim",
        &error,
        &reply,
        "s",
        username_.c_str());

    sd_bus_message_unref(reply);

    if (r < 0) {
        if (sd_bus_error_has_name(
                &error,
                FPRINTD_ERROR_ALREADY_IN_USE)) {

            result_ = Result::Busy;
        } else {
            result_ = Result::Error;
        }

        sd_bus_error_free(&error);
        return false;
    }

    sd_bus_error_free(&error);

    claimed_ = true;

    return true;
}


bool FprintdAuthenticator::verify_start()
{
    /*
     * Install signal handlers BEFORE VerifyStart().
     *
     * This is important because VerifyStatus may arrive very
     * quickly for some devices.
     */
    int r = sd_bus_match_signal(
        bus_,
        &verify_status_slot_,
        FPRINTD_BUS,
        device_.c_str(),
        FPRINTD_DEVICE_IFACE,
        "VerifyStatus",
        verify_status,
        this);

    if (r < 0) {
        return false;
    }

    r = sd_bus_match_signal(
        bus_,
        &verify_finger_selected_slot_,
        FPRINTD_BUS,
        device_.c_str(),
        FPRINTD_DEVICE_IFACE,
        "VerifyFingerSelected",
        verify_finger_selected,
        this);

    if (r < 0) {
        return false;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    r = sd_bus_call_method(
        bus_,
        FPRINTD_BUS,
        device_.c_str(),
        FPRINTD_DEVICE_IFACE,
        "VerifyStart",
        &error,
        &reply,
        "s",
        "any");

    sd_bus_message_unref(reply);

    if (r < 0) {
        if (sd_bus_error_has_name(
                &error,
                FPRINTD_ERROR_NO_ENROLLED_PRINTS)) {

            result_ = Result::NoEnrolledPrints;
        } else if (sd_bus_error_has_name(
                       &error,
                       FPRINTD_ERROR_ALREADY_IN_USE)) {

            result_ = Result::Busy;
        } else {
            result_ = Result::Error;
        }

        sd_bus_error_free(&error);
        return false;
    }

    sd_bus_error_free(&error);

    verify_started_ = true;

    return true;
}


bool FprintdAuthenticator::process_events(
    std::chrono::steady_clock::time_point deadline)
{
    std::fprintf(
        stderr,
        "[fprintd] entering event loop\n");
    while (!finished_ && !cancelled_) {

        /*
         * Process events which are already queued.
         */
        for (;;) {
            const int r = sd_bus_process(bus_, nullptr);

            if (r < 0) {
                result_ = Result::Disconnected;
                return false;
            }

            if (r == 0)
                break;

            if (finished_ || cancelled_)
                return true;
        }

        if (finished_ || cancelled_)
            break;

        const auto now =
            std::chrono::steady_clock::now();

        if (now >= deadline) {
            result_ = Result::Timeout;
            return false;
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - now);

        /*
         * sd_bus_wait() expects microseconds.
         */
        const int r = sd_bus_wait(
            bus_,
            remaining.count());

        if (r < 0) {
            if (r == -EINTR)
                continue;

            result_ = Result::Disconnected;
            return false;
        }
    }

    if (cancelled_) {
        result_ = Result::Cancelled;
        return false;
    }

    return finished_;
}


FprintdAuthenticator::Result
FprintdAuthenticator::authenticate(std::chrono::milliseconds timeout)
{
    result_ = Result::Error;

    if (!connect())
        return result_;

    if (!get_default_device()) {
        result_ = Result::NoDevice;
        cleanup();
        return result_;
    }

    if (!claim()) {
        cleanup();
        return result_;
    }

    if (!verify_start()) {
        cleanup();
        return result_;
    }

    std::fprintf(
        stderr,
        "[fprintd] VerifyStart succeeded\n");

    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    process_events(deadline);

    /*
     * VerifyStop MUST happen before Release.
     */
    if (verify_started_) {
        verify_stop();
    }

    release();

    return result_;
}


void FprintdAuthenticator::verify_stop()
{
    if (!verify_started_ || !bus_)
        return;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    const int r = sd_bus_call_method(
        bus_,
        FPRINTD_BUS,
        device_.c_str(),
        FPRINTD_DEVICE_IFACE,
        "VerifyStop",
        &error,
        &reply,
        "");

    sd_bus_message_unref(reply);

    /*
     * VerifyStop errors are deliberately ignored here.
     *
     * fprintd can already have stopped the verification after
     * emitting the final VerifyStatus.
     */
    if (r < 0 &&
        !sd_bus_error_has_name(
            &error,
            FPRINTD_ERROR_NO_ACTION)) {

        /*
         * Optional debug logging here.
         */
    }

    sd_bus_error_free(&error);

    verify_started_ = false;
}


void FprintdAuthenticator::release()
{
    if (!claimed_ || !bus_)
        return;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    sd_bus_call_method(
        bus_,
        FPRINTD_BUS,
        device_.c_str(),
        FPRINTD_DEVICE_IFACE,
        "Release",
        &error,
        &reply,
        "");

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    claimed_ = false;
}


void FprintdAuthenticator::cleanup()
{
    if (!bus_)
        return;

    if (verify_started_)
        verify_stop();

    if (claimed_)
        release();

    sd_bus_slot_unref(verify_status_slot_);
    verify_status_slot_ = nullptr;

    sd_bus_slot_unref(verify_finger_selected_slot_);
    verify_finger_selected_slot_ = nullptr;

    sd_bus_slot_unref(name_owner_slot_);
    name_owner_slot_ = nullptr;
}


int FprintdAuthenticator::verify_status(
    sd_bus_message* message,
    void* userdata,
    sd_bus_error*)
{
    auto* self =
        static_cast<FprintdAuthenticator*>(userdata);

    const char* status = nullptr;
    int done = 0;

    const int r =
        sd_bus_message_read(
            message,
            "sb",
            &status,
            &done);

    if (r < 0 || status == nullptr) {
        self->result_ = Result::Error;
        self->finished_ = true;
        return 0;
    }

    std::fprintf(
        stderr,
        "[fprintd] VerifyStatus: '%s', done=%d\n",
        status,
        done);

    /*
     * Intermediate status.
     *
     * fprintd may emit several status messages while
     * verification is still active.
     */
    if (!done)
        return 0;

    if (std::strcmp(status, "verify-match") == 0) {

        self->result_ = Result::Success;

    } else if (std::strcmp(status, "verify-no-match") == 0) {

        self->result_ = Result::NoMatch;

    } else if (std::strcmp(status, "verify-disconnected") == 0) {

        self->result_ = Result::Disconnected;

    } else if (std::strcmp(status, "verify-unknown-error") == 0) {

        self->result_ = Result::Error;

    } else {

        self->result_ = Result::Error;
    }

    self->finished_ = true;

    return 0;
}

int FprintdAuthenticator::verify_finger_selected(
    sd_bus_message* message,
    void*,
    sd_bus_error*)
{
    const char* finger = nullptr;

    if (sd_bus_message_read(
            message,
            "s",
            &finger) < 0) {

        std::fprintf(
            stderr,
            "[fprintd] VerifyFingerSelected: read failed\n");

        return 0;
    }

    std::fprintf(
        stderr,
        "[fprintd] VerifyFingerSelected: %s\n",
        finger ? finger : "(null)");

    /*
     * Optional:
     *
     * "Place your finger..."
     *
     * Howdy kann das momentan vermutlich ignorieren.
     */

    return 0;
}

int FprintdAuthenticator::fprintd_name_owner_changed(
    sd_bus_message* message,
    void* userdata,
    sd_bus_error*)
{
    auto* self =
        static_cast<FprintdAuthenticator*>(userdata);

    const char* name = nullptr;
    const char* old_owner = nullptr;
    const char* new_owner = nullptr;

    if (sd_bus_message_read(
            message,
            "sss",
            &name,
            &old_owner,
            &new_owner) < 0) {

        return 0;
    }

    if (std::strcmp(
            name,
            FPRINTD_BUS) != 0) {

        return 0;
    }

    /*
     * fprintd disappeared.
     */
    if (new_owner == nullptr ||
        new_owner[0] == '\0') {

        self->result_ =
            FprintdAuthenticator::Result::Disconnected;

        self->finished_ = true;
    }

    return 0;
}

void FprintdAuthenticator::cancel()
{
    cancelled_ = true;
}
