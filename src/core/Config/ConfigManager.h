// ConfigManager.h - Application configuration manager
// Loads and saves a JSON config file via nlohmann/json and provides a typed
// access API: dotted-key resolution, arrays, typed getters/setters.
//
// The file is UTF-8 JSON with 2-space indent. Key resolution supports dotted
// notation (e.g., "display.refreshRate").

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

class ConfigManager {
public:
    /// Construct with path to config file (default: "config.json")
    explicit ConfigManager(const std::string& path = "config.json");

    /// Load config from file using the format-specific parser.
    /// Returns true on success; initialises empty and returns false on error.
    bool Load();

    /// Save current config to file using the format-specific parser.
    /// Returns true on success.
    bool Save() const;

    // ---- Typed getters with defaults ----

    /// Get a string value. Returns defaultVal if key not found.
    std::string GetString(const std::string& key,
                          const std::string& defaultVal = "") const;

    /// Get an integer value. Returns defaultVal if key missing or not a number.
    int GetInt(const std::string& key, int defaultVal = 0) const;

    /// Get a double value. Returns defaultVal if key missing or not a number.
    double GetDouble(const std::string& key, double defaultVal = 0.0) const;

    /// Get a boolean value. Returns defaultVal if key missing.
    bool GetBool(const std::string& key, bool defaultVal = false) const;

    // ---- Typed setters ----

    void SetString(const std::string& key, const std::string& value);
    void SetInt(const std::string& key, int value);
    void SetUint64(const std::string& key, uint64_t value);
    void SetDouble(const std::string& key, double value);
    void SetBool(const std::string& key, bool value);

    /// Append an element (object, array, or scalar) to a JSON array at the given dotted key.
    /// Creates the array as an empty array if it does not exist.
    void AppendToArray(const std::string& arrayKey, nlohmann::json element);

    /// Set an arbitrary JSON value (object, array, scalar) at the given dotted key.
    void SetJson(const std::string& key, nlohmann::json value);

    /// Returns true if Load() has been called successfully.
    bool IsLoaded() const { return loaded_; }

    /// The config file path (resolved).
    const std::string& GetPath() const { return path_; }

    /// Validate loaded config against expected schema.
    /// Returns list of errors. Empty = valid. Caller should use defaults for invalid fields.
    /// #5 — config validation
    std::vector<std::string> Validate() const;

private:
    /// Resolve a dotted key like "display.refreshRate" into nested json pointers.
    /// Returns pointer to the value, or nullptr if not found.
    /// Uses pointer (not iterator) to avoid cross-container comparison issues.
    const nlohmann::json* ResolveKey(const std::string& key) const;

    /// Access the internal nlohmann::json storage.
    nlohmann::json& GetData();
    const nlohmann::json& GetData() const;

    nlohmann::json data_;

    std::string  path_;
    bool         loaded_ = false;
};
