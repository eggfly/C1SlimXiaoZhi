#include "mcp_server.h"
#include "test_framework.h"

#include <string>
#include <vector>

namespace {

// Drives the singleton and captures whatever it sends back.
class McpHarness {
public:
    McpHarness() {
        McpServer::GetInstance().SetSendCallback(
            [this](const std::string& payload) { replies_.push_back(payload); });
    }

    std::string Send(const std::string& request) {
        replies_.clear();
        McpServer::GetInstance().ParseMessage(request);
        return replies_.empty() ? std::string() : replies_.back();
    }

    size_t reply_count() const { return replies_.size(); }

private:
    std::vector<std::string> replies_;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(InitializeAnnouncesTheProtocolVersion) {
    McpHarness harness;
    std::string reply = harness.Send(R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    CHECK(Contains(reply, "\"protocolVersion\":\"2024-11-05\""));
    CHECK(Contains(reply, "\"serverInfo\""));
    CHECK(Contains(reply, "\"id\":1"));
}

TEST(ToolsListReturnsRegisteredTools) {
    McpServer::GetInstance().AddTool(
        "test.echo", "Echo the input back.",
        {{"value", McpServer::Property::Type::kString, "text to echo", true, 0, 0, false}},
        [](const McpServer::Arguments& args) {
            return McpServer::ToolResult::Ok(args.GetString("value"));
        });

    McpHarness harness;
    std::string reply = harness.Send(R"({"jsonrpc":"2.0","method":"tools/list","id":2})");
    CHECK(Contains(reply, "test.echo"));
    CHECK(Contains(reply, "inputSchema"));
    CHECK(Contains(reply, "\"required\":[\"value\"]"));
}

TEST(ToolsCallRunsTheHandler) {
    McpHarness harness;
    std::string reply = harness.Send(
        R"({"jsonrpc":"2.0","method":"tools/call","id":3,)"
        R"("params":{"name":"test.echo","arguments":{"value":"hello"}}})");
    CHECK(Contains(reply, "\"text\":\"hello\""));
    CHECK(Contains(reply, "\"isError\":false"));
}

TEST(MissingRequiredArgumentIsRejectedBeforeTheHandlerRuns) {
    McpHarness harness;
    std::string reply = harness.Send(
        R"({"jsonrpc":"2.0","method":"tools/call","id":4,)"
        R"("params":{"name":"test.echo","arguments":{}}})");
    CHECK(Contains(reply, "\"error\""));
    CHECK(Contains(reply, "missing required argument"));
}

TEST(UnknownToolAndMethodProduceErrors) {
    McpHarness harness;
    std::string reply = harness.Send(
        R"({"jsonrpc":"2.0","method":"tools/call","id":5,)"
        R"("params":{"name":"nope","arguments":{}}})");
    CHECK(Contains(reply, "-32601"));

    reply = harness.Send(R"({"jsonrpc":"2.0","method":"does/not/exist","id":6})");
    CHECK(Contains(reply, "-32601"));
}

TEST(NotificationsGetNoReply) {
    McpHarness harness;
    harness.Send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK_EQ(harness.reply_count(), static_cast<size_t>(0));
}

TEST(MalformedPayloadIsIgnored) {
    McpHarness harness;
    harness.Send("{not json");
    CHECK_EQ(harness.reply_count(), static_cast<size_t>(0));
}
