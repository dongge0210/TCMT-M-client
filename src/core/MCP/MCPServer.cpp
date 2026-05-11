#include "MCPServer.h"
#include "../Utils/Logger.h"
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

namespace tcmt::mcp {

static const char* PROTOCOL_VERSION = "2024-11-05";
static const char* SERVER_NAME    = "tcmt-mcp";
static const char* SERVER_VERSION = "alpha-0.2";

MCPServer::MCPServer()  = default;
MCPServer::~MCPServer() { Stop(); }

void MCPServer::Stop() { running_ = false; }

void MCPServer::RegisterTool(const std::string& name, const std::string& desc, ToolHandler handler) {
    tools_.push_back({name, desc, std::move(handler)});
}

void MCPServer::Run() {
    running_ = true;
    initialized_ = false;

    // Disable stdout buffering so JSON-RPC lines flush immediately
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin),  _O_BINARY);
#endif

    std::string line;
    while (running_.load() && ReadLine(line)) {
        if (line.empty()) continue;

        nlohmann::json request;
        try {
            request = nlohmann::json::parse(line);
        } catch (...) {
            WriteMessage(ErrorResponse(-32700, "Parse error", nullptr));
            continue;
        }

        if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0") {
            WriteMessage(ErrorResponse(-32600, "Invalid Request — missing jsonrpc", request.contains("id") ? request["id"] : nlohmann::json(nullptr)));
            continue;
        }

        std::string method = request.value("method", "");
        nlohmann::json id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
        nlohmann::json params = request.contains("params") ? request["params"] : nlohmann::json::object();

        nlohmann::json response;

        try {
            if      (method == "initialize")            response = HandleInitialize(params, id);
            else if (method == "notifications/initialized") continue; // no response
            else if (method == "tools/list")            response = HandleToolsList(id);
            else if (method == "tools/call")            response = HandleToolsCall(params, id);
            else if (method == "shutdown") {
                nlohmann::json r;
                r["jsonrpc"] = "2.0";
                r["id"] = id;
                r["result"] = nlohmann::json::object();
                WriteMessage(r);
                running_ = false;
                continue;
            }
            else if (method == "ping")                  
                response = { {"jsonrpc","2.0"}, {"id",id}, {"result",{}} };
            else {
                response = ErrorResponse(-32601, "Method not found: " + method, id);
            }
        } catch (const std::exception& e) {
            response = ErrorResponse(-32603, std::string("Internal error: ") + e.what(), id);
        }

        if (!response.empty() && !response.is_null())
            WriteMessage(response);
    }
}

void MCPServer::WriteMessage(const nlohmann::json& msg) {
    std::cout << msg.dump() << std::endl;
}

bool MCPServer::ReadLine(std::string& line) {
    return static_cast<bool>(std::getline(std::cin, line));
}

bool MCPServer::CheckRateLimit() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - rateWindowStart_).count();
    if (elapsed > 1000) {
        rateWindowStart_ = now;
        rateCount_ = 0;
    }
    return ++rateCount_ <= RATE_LIMIT;
}

nlohmann::json MCPServer::ErrorResponse(int code, const std::string& message, const nlohmann::json& id) {
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"]["code"] = code;
    r["error"]["message"] = message;
    return r;
}

// --- MCP lifecycle ---

nlohmann::json MCPServer::HandleInitialize(const nlohmann::json&, const nlohmann::json& id) {
    initialized_ = true;
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"]["protocolVersion"] = PROTOCOL_VERSION;
    r["result"]["capabilities"]["tools"]["listChanged"] = false;
    r["result"]["serverInfo"]["name"]    = SERVER_NAME;
    r["result"]["serverInfo"]["version"] = SERVER_VERSION;
    return r;
}

nlohmann::json MCPServer::HandleToolsList(const nlohmann::json& id) {
    if (!initialized_.load()) return ErrorResponse(-32002, "Server not initialized", id);

    nlohmann::json tools = nlohmann::json::array();
    for (const auto& t : tools_) {
        nlohmann::json td;
        td["name"] = t.name;
        td["description"] = t.description;
        td["inputSchema"]["type"] = "object";
        td["inputSchema"]["properties"] = nlohmann::json::object();
        tools.push_back(td);
    }
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"]["tools"] = tools;
    return r;
}

nlohmann::json MCPServer::HandleToolsCall(const nlohmann::json& params, const nlohmann::json& id) {
    if (!initialized_.load()) return ErrorResponse(-32002, "Server not initialized", id);
    if (!CheckRateLimit())    return ErrorResponse(-32029, "Rate limit exceeded", id);

    std::string toolName = params.value("name", "");
    for (const auto& t : tools_) {
        if (t.name == toolName) {
            auto data = t.handler();
            nlohmann::json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            nlohmann::json content;
            content["type"] = "text";
            content["text"] = data.dump();
            r["result"]["content"] = nlohmann::json::array({content});
            return r;
        }
    }
    return ErrorResponse(-32000, "Tool not found: " + toolName, id);
}

} // namespace tcmt::mcp
