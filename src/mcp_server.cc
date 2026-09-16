#include "mcp_server.h"

#include <cstdlib>
#include <cstring>

#include "audio/audio_service.h"
#include "boards/board.h"
#include "display/display.h"
#include "platform/log.h"
#include "settings.h"
#include "system_info.h"

#define TAG "MCP"

namespace {

// Upstream pages tools/list so a small device never builds one huge document.
// Same behaviour here, for compatibility with servers that rely on the cursor.
constexpr size_t kMaxToolsPerPage = 6;

}  // namespace

bool McpServer::Arguments::GetBool(const std::string& name, bool fallback) const {
    const cJSON* item = cJSON_GetObjectItem(object_, name.c_str());
    if (item == nullptr) {
        return fallback;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) != 0;
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble != 0;
    }
    return fallback;
}

int McpServer::Arguments::GetInt(const std::string& name, int fallback) const {
    const cJSON* item = cJSON_GetObjectItem(object_, name.c_str());
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return atoi(item->valuestring);
    }
    return fallback;
}

std::string McpServer::Arguments::GetString(const std::string& name,
                                            const std::string& fallback) const {
    const cJSON* item = cJSON_GetObjectItem(object_, name.c_str());
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return fallback;
}

McpServer& McpServer::GetInstance() {
    static McpServer server;
    return server;
}

void McpServer::SetSendCallback(std::function<void(const std::string&)> callback) {
    send_callback_ = std::move(callback);
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const std::vector<Property>& properties, ToolHandler handler) {
    for (const auto& tool : tools_) {
        if (tool.name == name) {
            C1XZ_LOGW(TAG, "tool %s is already registered", name.c_str());
            return;
        }
    }
    tools_.push_back({name, description, properties, std::move(handler)});
    C1XZ_LOGI(TAG, "registered tool %s", name.c_str());
}

cJSON* McpServer::BuildToolSchema(const Tool& tool) const {
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "name", tool.name.c_str());
    cJSON_AddStringToObject(entry, "description", tool.description.c_str());

    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON* properties = cJSON_CreateObject();
    cJSON* required = cJSON_CreateArray();
    for (const auto& property : tool.properties) {
        cJSON* entry_property = cJSON_CreateObject();
        const char* type_name = "string";
        if (property.type == Property::Type::kBoolean) {
            type_name = "boolean";
        } else if (property.type == Property::Type::kInteger) {
            type_name = "integer";
        }
        cJSON_AddStringToObject(entry_property, "type", type_name);
        if (!property.description.empty()) {
            cJSON_AddStringToObject(entry_property, "description", property.description.c_str());
        }
        if (property.type == Property::Type::kInteger && property.has_range) {
            cJSON_AddNumberToObject(entry_property, "minimum", property.minimum);
            cJSON_AddNumberToObject(entry_property, "maximum", property.maximum);
        }
        cJSON_AddItemToObject(properties, property.name.c_str(), entry_property);
        if (property.required) {
            cJSON_AddItemToArray(required, cJSON_CreateString(property.name.c_str()));
        }
    }
    cJSON_AddItemToObject(schema, "properties", properties);
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(entry, "inputSchema", schema);
    return entry;
}

void McpServer::Reply(const cJSON* id, cJSON* result) {
    if (!send_callback_) {
        cJSON_Delete(result);
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    if (id != nullptr) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, 1));
    }
    cJSON_AddItemToObject(root, "result", result);

    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        send_callback_(text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

void McpServer::ReplyError(const cJSON* id, int code, const std::string& message) {
    if (!send_callback_) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    if (id != nullptr) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, 1));
    }
    cJSON* error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message.c_str());
    cJSON_AddItemToObject(root, "error", error);

    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        send_callback_(text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

void McpServer::HandleInitialize(const cJSON* id, const cJSON* params) {
    (void)params;
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
    cJSON* capabilities = cJSON_CreateObject();
    cJSON_AddItemToObject(capabilities, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", capabilities);
    cJSON* server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "name", SystemInfo::GetBoardName());
    cJSON_AddStringToObject(server_info, "version", C1XZ_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", server_info);
    Reply(id, result);
}

void McpServer::HandleToolsList(const cJSON* id, const cJSON* params) {
    size_t start = 0;
    if (params != nullptr) {
        const cJSON* cursor = cJSON_GetObjectItem(params, "cursor");
        if (cJSON_IsString(cursor) && cursor->valuestring != nullptr &&
            cursor->valuestring[0] != '\0') {
            start = static_cast<size_t>(atoi(cursor->valuestring));
        }
    }

    cJSON* result = cJSON_CreateObject();
    cJSON* array = cJSON_CreateArray();
    size_t index = start;
    for (; index < tools_.size() && index - start < kMaxToolsPerPage; ++index) {
        cJSON_AddItemToArray(array, BuildToolSchema(tools_[index]));
    }
    cJSON_AddItemToObject(result, "tools", array);
    if (index < tools_.size()) {
        cJSON_AddStringToObject(result, "nextCursor", std::to_string(index).c_str());
    }
    Reply(id, result);
}

void McpServer::HandleToolsCall(const cJSON* id, const cJSON* params) {
    if (params == nullptr) {
        ReplyError(id, -32602, "missing params");
        return;
    }
    const cJSON* name = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name)) {
        ReplyError(id, -32602, "missing tool name");
        return;
    }
    const cJSON* arguments = cJSON_GetObjectItem(params, "arguments");

    for (const auto& tool : tools_) {
        if (tool.name != name->valuestring) {
            continue;
        }
        // Check required arguments before calling, so a tool never has to.
        for (const auto& property : tool.properties) {
            if (!property.required) {
                continue;
            }
            if (arguments == nullptr ||
                cJSON_GetObjectItem(arguments, property.name.c_str()) == nullptr) {
                ReplyError(id, -32602, "missing required argument: " + property.name);
                return;
            }
        }

        ToolResult outcome;
        Arguments args(arguments);
        outcome = tool.handler(args);

        cJSON* result = cJSON_CreateObject();
        cJSON* content = cJSON_CreateArray();
        cJSON* text_item = cJSON_CreateObject();
        cJSON_AddStringToObject(text_item, "type", "text");
        cJSON_AddStringToObject(text_item, "text", outcome.text.c_str());
        cJSON_AddItemToArray(content, text_item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddBoolToObject(result, "isError", outcome.is_error);
        Reply(id, result);
        return;
    }

    ReplyError(id, -32601, std::string("unknown tool: ") + name->valuestring);
}

void McpServer::ParseMessage(const std::string& payload) {
    cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
    if (root == nullptr) {
        C1XZ_LOGE(TAG, "mcp payload is not valid JSON");
        return;
    }
    ParseMessage(root);
    cJSON_Delete(root);
}

void McpServer::ParseMessage(const cJSON* payload) {
    if (payload == nullptr) {
        return;
    }
    const cJSON* id = cJSON_GetObjectItem(payload, "id");
    const cJSON* method = cJSON_GetObjectItem(payload, "method");
    if (!cJSON_IsString(method)) {
        // A response to something we sent; nothing to do.
        return;
    }
    const cJSON* params = cJSON_GetObjectItem(payload, "params");
    const char* name = method->valuestring;

    if (strcmp(name, "initialize") == 0) {
        HandleInitialize(id, params);
    } else if (strcmp(name, "tools/list") == 0) {
        HandleToolsList(id, params);
    } else if (strcmp(name, "tools/call") == 0) {
        HandleToolsCall(id, params);
    } else if (strcmp(name, "notifications/initialized") == 0) {
        // Notification, no reply expected.
    } else {
        ReplyError(id, -32601, std::string("unknown method: ") + name);
    }
}

void McpServer::RegisterCommonTools() {
    auto& board = Board::GetInstance();

    AddTool("self.get_device_status",
            "Report the device's current status: audio volume, network, battery "
            "and free memory. Call this before answering questions about the device.",
            {}, [&board](const Arguments&) {
                cJSON* status = cJSON_CreateObject();

                cJSON* audio = cJSON_CreateObject();
                AudioCodec* codec = board.GetAudioCodec();
                cJSON_AddNumberToObject(audio, "volume", codec != nullptr ? codec->output_volume()
                                                                          : 0);
                cJSON_AddItemToObject(status, "audio_speaker", audio);

                cJSON* network = cJSON_CreateObject();
                cJSON_AddStringToObject(network, "type", "wifi");
                cJSON_AddBoolToObject(network, "connected", board.IsNetworkReady());
                std::string ssid = board.GetNetworkName();
                if (!ssid.empty()) {
                    cJSON_AddStringToObject(network, "ssid", ssid.c_str());
                    cJSON_AddNumberToObject(network, "rssi", board.GetNetworkRssi());
                }
                cJSON_AddItemToObject(status, "network", network);

                int percent = 0;
                bool charging = false;
                if (board.GetBatteryLevel(&percent, &charging)) {
                    cJSON* battery = cJSON_CreateObject();
                    cJSON_AddNumberToObject(battery, "level", percent);
                    cJSON_AddBoolToObject(battery, "charging", charging);
                    cJSON_AddItemToObject(status, "battery", battery);
                }

                cJSON* system = cJSON_CreateObject();
                cJSON_AddNumberToObject(system, "free_memory",
                                        static_cast<double>(SystemInfo::GetFreeHeapSize()));
                cJSON_AddNumberToObject(system, "process_rss",
                                        static_cast<double>(SystemInfo::GetProcessRss()));
                cJSON_AddNumberToObject(system, "uptime_seconds",
                                        static_cast<double>(SystemInfo::GetUptimeSeconds()));
                cJSON_AddItemToObject(status, "system", system);

                char* text = cJSON_PrintUnformatted(status);
                std::string json(text != nullptr ? text : "{}");
                if (text != nullptr) {
                    cJSON_free(text);
                }
                cJSON_Delete(status);
                return ToolResult::Ok(json);
            });

    Property volume;
    volume.name = "volume";
    volume.type = Property::Type::kInteger;
    volume.description = "Speaker volume, 0 to 100";
    volume.minimum = 0;
    volume.maximum = 100;
    volume.has_range = true;
    AddTool("self.audio_speaker.set_volume", "Set the speaker volume.", {volume},
            [&board](const Arguments& args) {
                int value = args.GetInt("volume", -1);
                if (value < 0 || value > 100) {
                    return ToolResult::Error("volume must be between 0 and 100");
                }
                AudioCodec* codec = board.GetAudioCodec();
                if (codec == nullptr) {
                    return ToolResult::Error("no audio output on this device");
                }
                codec->SetOutputVolume(value);
                Settings settings("audio", true);
                settings.SetInt("volume", value);
                return ToolResult::Ok("true");
            });

    Property message;
    message.name = "message";
    message.type = Property::Type::kString;
    message.description = "Text to show on the screen";
    AddTool("self.screen.show_message",
            "Show a short message on the device's screen. The screen is a "
            "296x152 black and white e-paper panel that takes about 0.7 seconds "
            "to refresh, so keep messages short and do not call this rapidly.",
            {message}, [&board](const Arguments& args) {
                Display* display = board.GetDisplay();
                if (display == nullptr) {
                    return ToolResult::Error("no display on this device");
                }
                display->ShowNotification(args.GetString("message"), 8000);
                return ToolResult::Ok("true");
            });
}
