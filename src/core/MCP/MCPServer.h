#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>

namespace tcmt::mcp {

class MCPServer {
public:
    MCPServer();
    ~MCPServer();

    // Blocking: reads from stdin, writes JSON-RPC to stdout
    void Run();
    void Stop();

    // Hardware collection callbacks (set before Run())
    using ToolHandler = std::function<nlohmann::json()>;
    void RegisterTool(const std::string& name, const std::string& description, ToolHandler handler);

private:
    void WriteMessage(const nlohmann::json& msg);
    bool ReadLine(std::string& line);
    bool CheckRateLimit();

    nlohmann::json HandleInitialize(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json HandleToolsList(const nlohmann::json& id);
    nlohmann::json HandleToolsCall(const nlohmann::json& params, const nlohmann::json& id);
    nlohmann::json ErrorResponse(int code, const std::string& message, const nlohmann::json& id);

    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    struct ToolDef {
        std::string name;
        std::string description;
        ToolHandler handler;
    };
    std::vector<ToolDef> tools_;

    // Rate limiting
    std::chrono::steady_clock::time_point rateWindowStart_;
    int rateCount_ = 0;
    static constexpr int RATE_LIMIT = 30;
};

} // namespace tcmt::mcp
