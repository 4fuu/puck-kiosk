#include "app/plugin_ui.h"

#include "core/log.h"

#include <algorithm>
#include <cstddef>

namespace pk {
namespace {

std::string string_field(const nlohmann::json& value, const char* key) {
    if (value.contains(key) && value[key].is_string()) {
        return value[key].get<std::string>();
    }
    return {};
}

const nlohmann::json* object_field(const nlohmann::json& value, const char* key) {
    if (value.contains(key) && value[key].is_object()) {
        return &value[key];
    }
    return nullptr;
}

}  // namespace

nlohmann::json PluginUi::response_message(const std::string& request_id, int counter,
                                          const nlohmann::json* result) {
    nlohmann::json params = {{"requestId", request_id}};
    if (result) {
        params["result"] = *result;
    }
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", "executor_plugin_message"},
        {"params",
         {{"message",
           {{"type", "request"},
            {"id", "plugin-ui-respond-" + request_id + "-" + std::to_string(counter)},
            {"method", "plugin.ui.respond"},
            {"params", params}}}}},
    };
}

void PluginUi::on_actor_message(const std::string& text) {
    const nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
    if (message.is_discarded()) {
        return;
    }
    if (message.is_array()) {
        for (const nlohmann::json& entry : message) {
            handle_message(entry);
        }
        return;
    }
    handle_message(message);
}

void PluginUi::handle_message(const nlohmann::json& message) {
    if (!message.is_object()) {
        return;
    }
    const nlohmann::json* payload = &message;
    const nlohmann::json* params = object_field(message, "params");
    if (params) {
        payload = params;
    }

    // The actor delivers plugin events as plugin_message notifications. The
    // event object itself can also be handed in directly by tests.
    const nlohmann::json* event = payload;
    if (payload->contains("message") && (*payload)["message"].is_object()) {
        event = &(*payload)["message"];
    }
    if (!event->contains("type") || string_field(*event, "type") != "event") {
        return;
    }
    const std::string name = string_field(*event, "event");
    const nlohmann::json* data = object_field(*event, "data");
    if (name == "plugin.ui.snapshot") {
        if (data && data->contains("requests") && (*data)["requests"].is_array()) {
            apply_snapshot((*data)["requests"]);
        }
        return;
    }
    if (name == "plugin.ui.request_added" || name == "plugin.ui.request_settled") {
        if (data && data->contains("request")) {
            upsert((*data)["request"]);
        }
        return;
    }
    if (name == "plugin.ui.request_removed") {
        if (data) {
            remove(string_field(*data, "requestId"));
        }
        return;
    }
    if (name == "observer_snapshot") {
        // The web also rebuilds the request list from observerSnapshot.plugins.
        const nlohmann::json* plugins = object_field(*payload, "plugins");
        if (!plugins && data) {
            plugins = object_field(*data, "plugins");
        }
        if (plugins && plugins->contains("uiRequests") && (*plugins)["uiRequests"].is_array()) {
            apply_snapshot((*plugins)["uiRequests"]);
        }
    }
}

void PluginUi::apply_snapshot(const nlohmann::json& requests) {
    requests_.clear();
    order_.clear();
    for (const nlohmann::json& request : requests) {
        if (!request.is_object()) {
            continue;
        }
        const std::string id = string_field(request, "requestId");
        if (id.empty()) {
            continue;
        }
        Request parsed;
        parsed.id = id;
        parsed.method = string_field(request, "method");
        if (request.contains("params") && request["params"].is_object()) {
            parsed.params = request["params"];
        }
        parsed.status = string_field(request, "status");
        if (parsed.status.empty()) {
            parsed.status = "pending";
        }
        requests_[id] = std::move(parsed);
        order_.push_back(id);
    }
    log_sheet();
}

void PluginUi::upsert(const nlohmann::json& request) {
    if (!request.is_object()) {
        return;
    }
    const std::string id = string_field(request, "requestId");
    if (id.empty()) {
        return;
    }
    Request parsed;
    parsed.id = id;
    parsed.method = string_field(request, "method");
    if (request.contains("params") && request["params"].is_object()) {
        parsed.params = request["params"];
    }
    parsed.status = string_field(request, "status");
    if (parsed.status.empty()) {
        parsed.status = "pending";
    }
    if (!requests_.count(id)) {
        order_.push_back(id);
    } else if (parsed.status == "pending") {
        // A settled event may omit params; keep the options we already have.
        const Request& previous = requests_[id];
        if (!parsed.params.contains("options") && previous.params.contains("options")) {
            parsed.params["options"] = previous.params["options"];
        }
    }
    requests_[id] = std::move(parsed);
    log_sheet();
}

void PluginUi::remove(const std::string& request_id) {
    if (request_id.empty() || !requests_.count(request_id)) {
        return;
    }
    requests_.erase(request_id);
    order_.erase(std::remove(order_.begin(), order_.end(), request_id), order_.end());
    hidden_.erase(request_id);
    log_sheet();
}

const PluginUi::Request* PluginUi::visible_request() const {
    for (const std::string& id : order_) {
        auto it = requests_.find(id);
        if (it == requests_.end()) {
            continue;
        }
        const Request& request = it->second;
        if (request.resolved || request.status != "pending" || hidden_.count(id)) {
            continue;
        }
        return &request;
    }
    return nullptr;
}

PluginSheet PluginUi::sheet() const {
    PluginSheet view;
    const Request* request = visible_request();
    if (!request) {
        return view;
    }
    view.request_id = request->id;
    const nlohmann::json* options =
        request->params.contains("options") && request->params["options"].is_object()
            ? &request->params["options"]
            : nullptr;
    view.title = options ? string_field(*options, "title") : std::string{};
    view.message = options ? string_field(*options, "message") : std::string{};

    if (request->method == "ui.select" && options && options->contains("options") &&
        (*options)["options"].is_array()) {
        view.kind = PluginSheetKind::Select;
        for (const nlohmann::json& option : (*options)["options"]) {
            if (option.is_string()) {
                view.options.push_back(option.get<std::string>());
            }
        }
        if (view.title.empty()) {
            view.title = "Choose an option";
        }
        return view;
    }

    const bool has_fields = options && options->contains("fields");
    if (request->method == "ui.confirm" && !has_fields) {
        view.kind = PluginSheetKind::Confirm;
        if (options) {
            const std::string label = string_field(*options, "confirmButtonText");
            if (!label.empty()) {
                view.confirm_label = label;
            }
        }
        if (view.title.empty()) {
            view.title = "Confirm";
        }
        return view;
    }

    view.kind = PluginSheetKind::Phone;
    if (view.title.empty()) {
        view.title = "Puck needs an answer";
    }
    return view;
}

std::size_t PluginUi::pending_count() const {
    std::size_t count = 0;
    for (const auto& [id, request] : requests_) {
        if (!request.resolved && request.status == "pending" && !hidden_.count(id)) {
            ++count;
        }
    }
    return count;
}

bool PluginUi::respond(const Request& request, const nlohmann::json* result) {
    if (!sender_) {
        log::warn("plugin ui: no actor sender; cannot answer request %s", request.id.c_str());
        return false;
    }
    const nlohmann::json message =
        response_message(request.id, ++response_counter_, result);
    if (!sender_(message.dump())) {
        log::warn("plugin ui: actor send failed for request %s", request.id.c_str());
        return false;
    }
    return true;
}

bool PluginUi::select(int index) {
    const Request* request = visible_request();
    if (!request) {
        return false;
    }
    const PluginSheet view = sheet();
    if (view.kind != PluginSheetKind::Select || index < 0 ||
        index >= static_cast<int>(view.options.size())) {
        return false;
    }
    const nlohmann::json result = view.options[static_cast<std::size_t>(index)];
    const bool sent = respond(*request, &result);
    if (sent) {
        requests_[request->id].resolved = true;
        log::info("plugin ui: select %s = %s", request->id.c_str(),
                  result.get<std::string>().c_str());
    }
    return sent;
}

bool PluginUi::confirm(bool yes) {
    const Request* request = visible_request();
    if (!request || sheet().kind != PluginSheetKind::Confirm) {
        return false;
    }
    const nlohmann::json result = yes;
    const bool sent = respond(*request, &result);
    if (sent) {
        requests_[request->id].resolved = true;
        log::info("plugin ui: confirm %s = %s", request->id.c_str(), yes ? "yes" : "no");
    }
    return sent;
}

bool PluginUi::dismiss() {
    const Request* request = visible_request();
    if (!request) {
        return false;
    }
    const PluginSheet view = sheet();
    if (view.kind == PluginSheetKind::Select) {
        // The web cancels a select with plugin.ui.respond and no result.
        const bool sent = respond(*request, nullptr);
        if (sent) {
            requests_[request->id].resolved = true;
            log::info("plugin ui: dismissed select %s", request->id.c_str());
        }
        return sent;
    }
    hidden_.insert(request->id);
    log::info("plugin ui: hid %s sheet for %s", view.kind == PluginSheetKind::Confirm ? "confirm" : "phone",
              request->id.c_str());
    return true;
}

void PluginUi::phone_ok() {
    const Request* request = visible_request();
    if (!request || sheet().kind != PluginSheetKind::Phone) {
        return;
    }
    hidden_.insert(request->id);
    log::info("plugin ui: phone sheet acknowledged for %s", request->id.c_str());
}

void PluginUi::log_sheet() const {
    const PluginSheet view = sheet();
    if (view.kind == PluginSheetKind::None) {
        log::info("plugin ui: no pending requests (%zu tracked)", requests_.size());
        return;
    }
    log::info("plugin ui: %s request %s title=%s options=%zu",
              view.kind == PluginSheetKind::Select ? "select"
              : view.kind == PluginSheetKind::Confirm ? "confirm"
                                                      : "phone",
              view.request_id.c_str(), view.title.c_str(), view.options.size());
}

}  // namespace pk
