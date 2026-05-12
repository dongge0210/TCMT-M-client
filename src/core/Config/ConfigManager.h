// ConfigManager.h - Application configuration manager
// Wraps IConfigParser (cpp-parsers) for multi-format config file I/O
// and provides a typed access API backed by nlohmann/json internally.
//
// The IConfigParser is selected at construction based on file extension.
// Currently only the JSON backend is compiled, but the interface-based
// design allows future backends (YAML, TOML, XML, INI) to be swapped in
// without changing callers.
//
// If no matching parser is available for the file extension, a JsonConfigParser
// is used as a safe default.

#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

class IConfigParser;

class ConfigManager {
public:
    /// Construct with path to config file (default: "config.json")
    explicit ConfigManager(const std::string& path = "config.json");

    /// Destructor (required by unique_ptr<IConfigParser> with forward decl)
    ~ConfigManager();

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

    /// Access the internal nlohmann::json storage through the IConfigParser.
    /// Convenience accessors that downcast to JsonConfigParser.
    nlohmann::json& GetData();
    const nlohmann::json& GetData() const;

    /// Format-agnostic config parser (always a JsonConfigParser with
    /// the current build configuration).
    std::unique_ptr<IConfigParser> parser_;

    std::string  path_;
    bool         loaded_ = false;
};
