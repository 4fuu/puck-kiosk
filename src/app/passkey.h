#pragma once

#include <string>
#include <vector>

namespace pk {

enum class PasskeyMode {
    Hidden,
    Phone,
    Local,
    Confirming,
    Failed,
};

struct PasskeyStep {
    std::string title;
    std::string detail;
};

struct PasskeyContext {
    bool car_session = false;
    bool actor_connected = false;
    bool can_write = false;
    bool requires_sudo = false;
};

struct PasskeyView {
    PasskeyMode mode = PasskeyMode::Hidden;
    std::string title;
    std::vector<PasskeyStep> steps;
    bool confirm_button = false;
    bool confirming = false;
    std::string status;
};

// The car page's passkey guidance. The web shows the phone variant whenever the
// thread actor is read-only on a car session, and the local variant (with a
// Confirm Passkey button) when a browser page has no car session but the actor
// still needs sudo. The kiosk has no platform authenticator, so the local
// button asks the app to re-authorize the actor connection; the flow stays in
// Confirming until a later actor/session update reports write access.
class PasskeyFlow {
public:
    void set_context(const PasskeyContext& context);
    const PasskeyContext& context() const { return context_; }

    PasskeyView view() const;
    PasskeyMode mode() const { return mode_; }
    bool confirm_button() const {
        return mode_ == PasskeyMode::Local || mode_ == PasskeyMode::Failed;
    }
    bool confirming() const { return mode_ == PasskeyMode::Confirming; }

    // Enters Confirming when the local button is available. The app sends the
    // actor request when this returns true.
    bool confirm();

    // A response to the actor request the app sent for confirm().
    void on_request_result(bool ok, const std::string& error);

private:
    void rebuild();

    PasskeyContext context_;
    PasskeyMode mode_ = PasskeyMode::Hidden;
    std::string status_;
};

}  // namespace pk
