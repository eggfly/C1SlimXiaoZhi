#ifndef C1XZ_MCP_SERVER_H
#define C1XZ_MCP_SERVER_H

#include <cJSON.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Device-side Model Context Protocol server.
//
// Ported from xiaozhi-esp32 main/mcp_server.*. The transport wraps each message
// as {"session_id":..,"type":"mcp","payload":<JSON-RPC 2.0>}; this class only
// deals with the payload.
//
// Supported methods: initialize, tools/list (with cursor paging), tools/call.
// The protocol version reported is 2024-11-05, matching upstream.
class McpServer {
public:
    struct Property {
        enum class Type { kBoolean, kInteger, kString };
        std::string name;
        Type type = Type::kString;
        std::string description;
        bool required = true;
        // Bounds for integers; ignored otherwise.
        int minimum = 0;
        int maximum = 0;
        bool has_range = false;
    };

    // Arguments given to a tool, already validated against its properties.
    class Arguments {
    public:
        explicit Arguments(const cJSON* object) : object_(object) {}
        bool GetBool(const std::string& name, bool fallback = false) const;
        int GetInt(const std::string& name, int fallback = 0) const;
        std::string GetString(const std::string& name,
                              const std::string& fallback = std::string()) const;

    private:
        const cJSON* object_;
    };

    // A tool returns either plain text or a JSON document. Throwing is not an
    // option here, so failures come back as a message with is_error set.
    struct ToolResult {
        std::string text;
        bool is_error = false;
        static ToolResult Ok(const std::string& text) { return {text, false}; }
        static ToolResult Error(const std::string& text) { return {text, true}; }
    };

    using ToolHandler = std::function<ToolResult(const Arguments&)>;

    static McpServer& GetInstance();

    // Called with a complete JSON-RPC payload to send back to the server.
    void SetSendCallback(std::function<void(const std::string&)> callback);

    void AddTool(const std::string& name, const std::string& description,
                 const std::vector<Property>& properties, ToolHandler handler);

    // Registers the tools that every board has. Boards add their own on top.
    void RegisterCommonTools();

    // Entry point for an incoming mcp payload.
    void ParseMessage(const cJSON* payload);
    void ParseMessage(const std::string& payload);

private:
    struct Tool {
        std::string name;
        std::string description;
        std::vector<Property> properties;
        ToolHandler handler;
    };

    std::vector<Tool> tools_;
    std::function<void(const std::string&)> send_callback_;

    void Reply(const cJSON* id, cJSON* result);
    void ReplyError(const cJSON* id, int code, const std::string& message);
    void HandleInitialize(const cJSON* id, const cJSON* params);
    void HandleToolsList(const cJSON* id, const cJSON* params);
    void HandleToolsCall(const cJSON* id, const cJSON* params);
    cJSON* BuildToolSchema(const Tool& tool) const;
};

#endif  // C1XZ_MCP_SERVER_H
