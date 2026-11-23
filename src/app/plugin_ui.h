#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pk {

enum class PluginSheetKind {
    None,
    Select,
    Confirm,
    Phone,
};

// The car page's view of one pending plugin UI request. The web maps
// ui.select to option buttons, ui.confirm without fields to Yes/No, and
// everything else (ui.input, confirm with fields) to the phone sheet.
struct PluginSheet {
    PluginSheetKind kind = PluginSheetKind::None;
    std::string request_id;
    std::string title;
    std::string message;
    std::vector<std::string> options;
    std::string confirm_label = "Yes";
};

// Owns plugin.ui.* actor messages and answers them with plugin.ui.respond.
//
// Actor wire shapes (confirmed from the web client):
//   in : {"jsonrpc":"2.0","method":"plugin_message","params":{
//           "type":"event","event":"plugin.ui.snapshot","data":{"requests":[..]}}}
//   out: {"jsonrpc":"2.0","method":"executor_plugin_message","params":{
//           "message":{"type":"request","id":"plugin-ui-respond-<id>-<n>",
//                      "method":"plugin.ui.respond","params":{"requestId":...,
//                      "result":...}}}}
class PluginUi {
public:
    using Sender = std::function<bool(const std::string&)>;

    void set_sender(Sender sender) { sender_ = std::move(sender); }

    void on_actor_message(const std::string& text);

    // The first pending, not-dismissed request, in arrival order.
    PluginSheet sheet() const;
    bool visible() const { return sheet().kind != PluginSheetKind::None; }
    std::size_t pending_count() const;

    // Answers the visible sheet. A select answer and Yes/No send a response;
    // Dismiss on a select sends a response without a result; Dismiss on a
    // confirm and OK on a phone sheet only hide the sheet, as on the web.
    bool select(int index);
    bool confirm(bool yes);
    bool dismiss();
    void phone_ok();

    // Exposed for tests and logging.
    static nlohmann::json response_message(const std::string& request_id, int counter,
                                           const nlohmann::json* result);

private:
    struct Request {
        std::string id;
        std::string method;
        nlohmann::json params;
        std::string status = "pending";
        bool resolved = false;
    };

    void handle_message(const nlohmann::json& message);
    void apply_snapshot(const nlohmann::json& requests);
    void upsert(const nlohmann::json& request);
    void remove(const std::string& request_id);
    const Request* visible_request() const;
    bool respond(const Request& request, const nlohmann::json* result);
    void log_sheet() const;

    std::map<std::string, Request> requests_;
    std::vector<std::string> order_;
    std::set<std::string> hidden_;
    Sender sender_;
    int response_counter_ = 0;
};

}  // namespace pk
