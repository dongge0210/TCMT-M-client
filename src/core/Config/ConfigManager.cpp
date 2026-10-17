// ConfigManager.cpp - Application configuration manager
// File I/O is plain JSON: load parses the file into nlohmann::json, save
// dumps it with 2-space indent. Dotted-key resolution ("display.refreshRate")
// and the typed access API live directly on the nlohmann::json document.

#include "ConfigManager.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

// =========================================================================
// Helper: split dotted key "foo.bar.baz" -> ["foo", "bar", "baz"]
// =========================================================================
static std::vector<std::string> SplitKey(const std::string& key) {
    std::vector<std::string> parts;
    std::stringstream ss(key);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (!part.empty())
            parts.push_back(part);
    }
    return parts;
}

// =========================================================================
// Construction / Destruction
// =========================================================================

ConfigManager::ConfigManager(const std::string& path)
    : path_(path)
{
}

// =========================================================================
// Accessors to the internal nlohmann::json document
// =========================================================================

nlohmann::json& ConfigManager::GetData() {
    return data_;
}

const nlohmann::json& ConfigManager::GetData() const {
    return data_;
}

// =========================================================================
// Loading / Saving (plain JSON file I/O)
// =========================================================================

bool ConfigManager::Load() {
    try {
        std::ifstream in(path_);
        if (!in) {
            // File doesn't exist yet — that's fine, start with empty config
            data_ = nlohmann::json::object();
            loaded_ = true;
            return true;
        }
        in >> data_;
    } catch (const nlohmann::json::parse_error&) {
        // Corrupt JSON file — reset to empty
        data_ = nlohmann::json::object();
        loaded_ = true;
        return false;
    }
    loaded_ = true;
    return true;
}

bool ConfigManager::Save() const {
    if (!loaded_) {
        // Nothing to save — config was never loaded
        return false;
    }
    try {
        std::ofstream out(path_);
        if (!out) return false;
        out << data_.dump(2);
        return out.good();
    } catch (...) {
        return false;
    }
}

// =========================================================================
// Key resolution (dotted notation)
// =========================================================================

const nlohmann::json* ConfigManager::ResolveKey(
    const std::string& key) const
{
    auto parts = SplitKey(key);
    if (parts.empty()) {
        return nullptr;
    }

    // Walk the JSON tree
    const nlohmann::json* current = &GetData();
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        if (!current->is_object()) return nullptr;
        auto it = current->find(parts[i]);
        if (it == current->end() || !it->is_object()) {
            return nullptr;
        }
        current = &it.value();
    }

    // Final part — use pointer check instead of iterator comparison
    if (!current->is_object()) return nullptr;
    auto it = current->find(parts.back());
    if (it == current->end()) return nullptr;
    return &it.value();
}

// =========================================================================
// Getters
// =========================================================================

std::string ConfigManager::GetString(const std::string& key,
                                     const std::string& defaultVal) const
{
    if (!loaded_) return defaultVal;
    auto* val = ResolveKey(key);
    if (val == nullptr || !val->is_string())
        return defaultVal;
    return val->get<std::string>();
}

int ConfigManager::GetInt(const std::string& key, int defaultVal) const {
    if (!loaded_) return defaultVal;
    auto* val = ResolveKey(key);
    if (val == nullptr || !val->is_number_integer())
        return defaultVal;
    return val->get<int>();
}

double ConfigManager::GetDouble(const std::string& key,
                                double defaultVal) const {
    if (!loaded_) return defaultVal;
    auto* val = ResolveKey(key);
    if (val == nullptr || !val->is_number_float())
        return defaultVal;
    return val->get<double>();
}

bool ConfigManager::GetBool(const std::string& key,
                            bool defaultVal) const {
    if (!loaded_) return defaultVal;
    auto* val = ResolveKey(key);
    if (val == nullptr || !val->is_boolean())
        return defaultVal;
    return val->get<bool>();
}

// =========================================================================
// Setters
// =========================================================================

static void SetNested(nlohmann::json& root,
                      const std::string& key,
                      nlohmann::json&& value)
{
    auto parts = SplitKey(key);
    if (parts.empty()) return;

    nlohmann::json* current = &root;
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        if (!current->is_object()) {
            *current = nlohmann::json::object();
        }
        auto it = current->find(parts[i]);
        if (it == current->end() || !it->is_object()) {
            (*current)[parts[i]] = nlohmann::json::object();
        }
        current = &(*current)[parts[i]];
    }
    (*current)[parts.back()] = std::move(value);
}

void ConfigManager::SetString(const std::string& key,
                              const std::string& value) {
    SetNested(GetData(), key, value);
}

void ConfigManager::SetInt(const std::string& key, int value) {
    SetNested(GetData(), key, value);
}

void ConfigManager::SetUint64(const std::string& key, uint64_t value) {
    SetNested(GetData(), key, value);
}

void ConfigManager::SetDouble(const std::string& key, double value) {
    SetNested(GetData(), key, value);
}

void ConfigManager::SetBool(const std::string& key, bool value) {
    SetNested(GetData(), key, value);
}

void ConfigManager::AppendToArray(const std::string& arrayKey,
                                  nlohmann::json element) {
    auto parts = SplitKey(arrayKey);
    if (parts.empty()) return;

    nlohmann::json* current = &GetData();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!current->is_object()) {
            *current = nlohmann::json::object();
        }
        auto it = current->find(parts[i]);
        if (it == current->end()) {
            if (i == parts.size() - 1) {
                (*current)[parts[i]] = nlohmann::json::array();
            } else {
                (*current)[parts[i]] = nlohmann::json::object();
            }
        }
        current = &(*current)[parts[i]];
    }

    if (!current->is_array()) {
        *current = nlohmann::json::array();
    }
    current->push_back(std::move(element));
}

void ConfigManager::SetJson(const std::string& key, nlohmann::json value) {
    SetNested(GetData(), key, std::move(value));
}

std::vector<std::string> ConfigManager::Validate() const {
    std::vector<std::string> errors;

    // Define expected schema: {key, type, validator}
    struct Rule { const char* key; enum { STR, INT, DOUBLE, BOOL } type;
                  double minV = 0, maxV = 0; const char* const* enumVals = nullptr; int enumN = 0; };
    static const char* logLevels[] = {"debug", "info", "warning", "error"};
    Rule rules[] = {
        {"logging.level",         Rule::STR,    0, 0, logLevels, 4},
        {"display.refreshRate",   Rule::INT,   100, 5000},
        {"sampling.intervalMs",   Rule::INT,   200, 5000},
    };

    for (auto& r : rules) {
        auto* v = ResolveKey(r.key);
        if (!v || v->is_null()) continue; // missing → use default, not an error

        switch (r.type) {
        case Rule::STR:
            if (!v->is_string()) { errors.push_back(std::string(r.key) + ": expected string"); break; }
            if (r.enumVals) {
                bool ok = false;
                for (int i = 0; i < r.enumN; ++i)
                    if (v->get<std::string>() == r.enumVals[i]) { ok = true; break; }
                if (!ok) errors.push_back(std::string(r.key) + ": invalid value '" + v->get<std::string>() + "'");
            }
            break;
        case Rule::INT:
            if (!v->is_number_integer()) { errors.push_back(std::string(r.key) + ": expected integer"); break; }
            if (r.maxV > r.minV) { int iv = v->get<int>(); if (iv < r.minV || iv > r.maxV) errors.push_back(std::string(r.key) + ": out of range [" + std::to_string((int)r.minV) + "-" + std::to_string((int)r.maxV) + "]"); }
            break;
        case Rule::DOUBLE:
            if (!v->is_number()) { errors.push_back(std::string(r.key) + ": expected number"); break; }
            if (r.maxV > r.minV) { double dv = v->get<double>(); if (dv < r.minV || dv > r.maxV) errors.push_back(std::string(r.key) + ": out of range"); }
            break;
        case Rule::BOOL:
            if (!v->is_boolean()) errors.push_back(std::string(r.key) + ": expected boolean");
            break;
        }
    }
    return errors;
}
