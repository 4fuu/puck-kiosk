#include "app/passkey.h"

#include "core/log.h"

namespace pk {

void PasskeyFlow::set_context(const PasskeyContext& context) {
    context_ = context;

    if (!context_.actor_connected || context_.can_write) {
        if (mode_ != PasskeyMode::Hidden) {
            log::info("passkey: hidden (actor %s, capability %s)",
                      context_.actor_connected ? "connected" : "disconnected",
                      context_.can_write ? "write" : "read");
        }
        mode_ = PasskeyMode::Hidden;
        status_.clear();
        return;
    }
    if (mode_ == PasskeyMode::Confirming || mode_ == PasskeyMode::Failed) {
        return;
    }

    if (context_.car_session) {
        mode_ = PasskeyMode::Phone;
    } else if (context_.requires_sudo) {
        mode_ = PasskeyMode::Local;
    } else {
        mode_ = PasskeyMode::Hidden;
    }
}

PasskeyView PasskeyFlow::view() const {
    PasskeyView view;
    view.mode = mode_;
    view.confirming = mode_ == PasskeyMode::Confirming;
    view.confirm_button = mode_ == PasskeyMode::Local || mode_ == PasskeyMode::Failed;
    view.status = status_;
    switch (mode_) {
        case PasskeyMode::Hidden:
            break;
        case PasskeyMode::Phone:
            view.title = "Confirm Passkey on Your Phone";
            view.steps = {
                {"Open your phone\u2019s browser",
                 "Go to the tab where you signed in this car, not the Amp app."},
                {"Tap Confirm Passkey",
                 "Your phone asks for Face ID, Touch ID, or your passcode."},
                {"The call button turns green",
                 "In about 10 seconds. Tap it to call Puck."},
            };
            break;
        case PasskeyMode::Local:
        case PasskeyMode::Confirming:
        case PasskeyMode::Failed:
            view.title = "Confirm Your Passkey";
            view.steps = {
                {"Tap Confirm Passkey below", ""},
                {"Approve the passkey prompt",
                 "If it shows a QR code, scan it with your phone\u2019s camera and approve there."},
                {"The call button appears here", "Tap it to call Puck."},
            };
            break;
    }
    return view;
}

bool PasskeyFlow::confirm() {
    if (mode_ != PasskeyMode::Local && mode_ != PasskeyMode::Failed) {
        return false;
    }
    mode_ = PasskeyMode::Confirming;
    status_ = "Waiting for the passkey confirmation";
    log::info("passkey: confirm requested");
    return true;
}

void PasskeyFlow::on_request_result(bool ok, const std::string& error) {
    if (mode_ != PasskeyMode::Confirming) {
        return;
    }
    if (ok) {
        status_ = "Waiting for the passkey confirmation";
        log::info("passkey: actor accepted the confirmation request");
        return;
    }
    mode_ = PasskeyMode::Failed;
    status_ = error.empty() ? "The passkey request failed" : error;
    log::warn("passkey: %s", status_.c_str());
}

}  // namespace pk
