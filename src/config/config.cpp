#include "maxcompute_odbc/common/logging.h"
#include "maxcompute_odbc/common/utils.h"
#include "maxcompute_odbc/config/config.h"
#include "maxcompute_odbc/version.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace maxcompute_odbc {
void Config::validate() const {
  // Validate required parameters
  if (endpoint.empty()) {
    throw std::invalid_argument("Endpoint is required");
  }

  if (project.empty()) {
    throw std::invalid_argument("Project is required");
  }

  // Either accessKeyId/accessKeySecret pair or credential client is required
  bool hasAccessKeyId = accessKeyId.has_value() && !accessKeyId->empty();
  bool hasAccessKeySecret =
      accessKeySecret.has_value() && !accessKeySecret->empty();

  if (hasAccessKeyId != hasAccessKeySecret) {
    throw std::invalid_argument(
        "Both accessKeyId and accessKeySecret must be provided together");
  }

  if (!hasAccessKeyId && !credential) {
    throw std::invalid_argument(
        "Either accessKeyId/accessKeySecret or credential client must be "
        "provided");
  }

  if (readTimeout < 0) {
    throw std::invalid_argument("Read timeout must be non-negative");
  }

  if (connectTimeout < 0) {
    throw std::invalid_argument("Connect timeout must be non-negative");
  }
}

bool Config::hasValidCredentials() const {
  // Check if we have either accessKeyId/accessKeySecret or credential client
  bool hasAccessKeyId = accessKeyId.has_value() && !accessKeyId->empty();
  bool hasAccessKeySecret =
      accessKeySecret.has_value() && !accessKeySecret->empty();
  return (hasAccessKeyId && hasAccessKeySecret) || (credential != nullptr);
}

std::shared_ptr<ICredentialsProvider> Config::getCredentialsProvider() const {
  bool hasAccessKeyId = accessKeyId.has_value() && !accessKeyId->empty();
  bool hasAccessKeySecret =
      accessKeySecret.has_value() && !accessKeySecret->empty();

  if (hasAccessKeyId && hasAccessKeySecret) {
    return std::make_shared<StaticCredentialProvider>(
        accessKeyId.value(), accessKeySecret.value(),
        securityToken.value_or(""));
  } else if (credential) {
    // Use existing credential client
    return credential;
  }
  throw std::invalid_argument("Credential config must be set.");
}

std::string Config::getFullEndpoint() const {
  // If endpoint already starts with http:// or https://, return as is
  if (endpoint.find("http://") == 0 || endpoint.find("https://") == 0) {
    return endpoint;
  }

  // Otherwise, prepend the protocol
  return protocol + "://" + endpoint;
}

// Helper function to trim whitespace from string
static std::string trim(const std::string &str) {
  const auto strBegin = str.find_first_not_of(" \t");
  if (strBegin == std::string::npos) return "";

  const auto strEnd = str.find_last_not_of(" \t");
  const auto strRange = strEnd - strBegin + 1;

  return str.substr(strBegin, strRange);
}

// Helper function to convert string to lowercase
static std::string toLower(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return str;
}

// ODBC values may be bare, double quoted, or enclosed in braces. Keep the
// ordered attributes so duplicate keys retain their existing last-value wins
// behavior in both configuration entry points.
static std::vector<std::pair<std::string, std::string>> parseAttributes(
    const std::string& input) {
  std::vector<std::pair<std::string, std::string>> attributes;
  size_t pos = 0;
  while (pos < input.size()) {
    while (pos < input.size() &&
           (input[pos] == ';' ||
            std::isspace(static_cast<unsigned char>(input[pos]))))
      ++pos;
    if (pos == input.size()) break;
    const size_t equals = input.find('=', pos);
    const size_t separator = input.find(';', pos);
    if (equals == std::string::npos ||
        (separator != std::string::npos && separator < equals)) {
      throw std::invalid_argument("Invalid connection string attribute");
    }
    const std::string key = trim(input.substr(pos, equals - pos));
    if (key.empty()) throw std::invalid_argument("Empty connection string key");
    pos = equals + 1;
    while (pos < input.size() &&
           std::isspace(static_cast<unsigned char>(input[pos])))
      ++pos;
    std::string value;
    // Preserve the driver's existing unquoted JSON GlobalSettings syntax.
    size_t json_start = pos + 1;
    while (json_start < input.size() &&
           std::isspace(static_cast<unsigned char>(input[json_start])))
      ++json_start;
    const bool raw_json =
        toLower(key) == "globalsettings" && pos < input.size() &&
        input[pos] == '{' && json_start < input.size() &&
        (input[json_start] == '"' || input[json_start] == '}');
    if (pos < input.size() &&
        (input[pos] == '"' || (input[pos] == '{' && !raw_json))) {
      const bool braced = input[pos++] == '{';
      const char closing = braced ? '}' : '"';
      bool closed = false;
      while (pos < input.size()) {
        const char c = input[pos++];
        if (!braced && c == '\\' && pos < input.size() && input[pos] == '"') {
          value += input[pos++];
        } else if (c == closing) {
          if (pos < input.size() && input[pos] == closing) {
            value += input[pos++];
          } else {
            closed = true;
            break;
          }
        } else {
          value += c;
        }
      }
      if (!closed)
        throw std::invalid_argument("Unclosed connection string value");
      while (pos < input.size() &&
             std::isspace(static_cast<unsigned char>(input[pos])))
        ++pos;
      if (pos < input.size() && input[pos] != ';') {
        throw std::invalid_argument(
            "Unexpected text after connection string value");
      }
      if (pos < input.size()) ++pos;
    } else {
      const size_t end = input.find(';', pos);
      value =
          trim(input.substr(pos, end == std::string::npos ? end : end - pos));
      pos = end == std::string::npos ? input.size() : end + 1;
    }
    attributes.emplace_back(key, std::move(value));
  }
  return attributes;
}

static std::string escapeConnectionValue(const std::string& value) {
  if (value.find_first_of(";{}\"\r\n") == std::string::npos &&
      value == trim(value))
    return value;
  std::string escaped = "{";
  for (char c : value) {
    escaped += c;
    if (c == '}') escaped += '}';
  }
  return escaped + "}";
}

std::string Config::buildOutputConnectionString() const {
  // The completed string intentionally excludes passwords and security tokens.
  std::string output = "DRIVER={MaxCompute ODBC Driver};SERVER=" +
                       escapeConnectionValue(endpoint) +
                       ";PROJECT=" + escapeConnectionValue(project);
  if (accessKeyId) output += ";UID=" + escapeConnectionValue(*accessKeyId);
  output += ";ClientCharset=" + escapeConnectionValue(clientCharset) + ";";
  return output;
}

std::string Config::encodingDiagnostics() const {
  return std::string("driver=") + kDriverVersion +
         "; ClientCharset=" + clientCharset + "; source=" +
         (clientCharsetSpecified ? "connection-string" : "default") +
         "; Unicode=UTF-16";
}

std::map<std::string, std::string>
ConnectionStringParser::parseDriverConnectString(const std::string &conn_str) {
  std::map<std::string, std::string> params;
  for (const auto& [key, value] : parseAttributes(conn_str)) {
    std::string upper = key;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    params[upper] = value;
  }
  return params;
}

Config ConnectionStringParser::parse(const std::string &connStr) {
  Config config;
  for (const auto& [key, value] : parseAttributes(connStr)) {
    const std::string lowerKey = toLower(key);
    if (lowerKey == "endpoint" || lowerKey == "server") {
      config.endpoint = value;
    } else if (lowerKey == "project") {
      config.project = value;
    } else if (lowerKey == "schema") {
      config.schema = value;
    } else if (lowerKey == "accesskeyid") {
      config.accessKeyId = value;
    } else if (lowerKey == "accesskeysecret") {
      config.accessKeySecret = value;
    } else if (lowerKey == "securitytoken") {
      config.securityToken = value;
    } else if (lowerKey == "readtimeout") {
      try {
        config.readTimeout = std::stoi(value);
      } catch (...) {
        config.readTimeout = 60;
      }
    } else if (lowerKey == "connecttimeout") {
      try {
        config.connectTimeout = std::stoi(value);
      } catch (...) {
        config.connectTimeout = 30;
      }
    } else if (lowerKey == "protocol") {
      config.protocol = toLower(value);
      if (config.protocol != "http" && config.protocol != "https") {
        config.protocol = "https";
      }
    } else if (lowerKey == "datasourcename") {
      config.dataSourceName = value;
    } else if (lowerKey == "regionid") {
      config.regionId = value;
    } else if (lowerKey == "useragent") {
      config.userAgent = value;
    } else if (lowerKey == "httpproxy") {
      config.httpProxy = value;
      auto parsed = parseProxyUrl(value);
      if (!std::get<2>(parsed).empty() && !config.proxyUser.has_value()) {
        config.proxyUser = std::get<2>(parsed);
      }
      if (!std::get<3>(parsed).empty() && !config.proxyPassword.has_value()) {
        config.proxyPassword = std::get<3>(parsed);
      }
    } else if (lowerKey == "httpsproxy") {
      config.httpsProxy = value;
      auto parsed = parseProxyUrl(value);
      if (!std::get<2>(parsed).empty() && !config.proxyUser.has_value()) {
        config.proxyUser = std::get<2>(parsed);
      }
      if (!std::get<3>(parsed).empty() && !config.proxyPassword.has_value()) {
        config.proxyPassword = std::get<3>(parsed);
      }
    } else if (lowerKey == "proxyuser") {
      config.proxyUser = value;
    } else if (lowerKey == "proxypassword" || lowerKey == "proxypwd") {
      config.proxyPassword = value;
    } else if (lowerKey == "logpath") {
      maxcompute_odbc::Logger::getInstance().setLogFile(value);
    } else if (lowerKey == "loglevel") {
      maxcompute_odbc::Logger::getInstance().setLogLevel(value);
    } else if (lowerKey == "timezone") {
      config.timezone = value;
    } else if (lowerKey == "namespaceschema") {
      config.namespaceSchema = value != "false";
    } else if (lowerKey == "clientcharset") {
      if (!value.empty()) {
        std::string normalized = trim(value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (normalized == "UTF8") normalized = "UTF-8";
        config.clientCharset = normalized;
        config.clientCharsetSpecified = true;
      }
    } else if (lowerKey == "fetchresultsplitsize") {
      try {
        config.fetchResultSplitSize = std::stoll(value);
      } catch (...) {
        config.fetchResultSplitSize = 10000;
      }
    } else if (lowerKey == "fetchresultpreloadsplitnum") {
      try {
        config.fetchResultPreloadSplitNum = std::stol(value);
      } catch (...) {
        config.fetchResultPreloadSplitNum = 5;
      }
    } else if (lowerKey == "fetchresultthreadnum") {
      try {
        config.fetchResultThreadNum = std::stol(value);
      } catch (...) {
        config.fetchResultThreadNum = 5;
      }
    } else if (lowerKey == "logviewhost") {
      config.logviewHost = value;
    } else if (lowerKey == "globalsettings") {
      try {
        auto json_obj = nlohmann::json::parse(value);
        if (json_obj.is_object()) {
          for (auto &item : json_obj.items()) {
            if (item.value().is_string()) {
              config.globalSettings[item.key()] =
                  item.value().get<std::string>();
            } else {
              config.globalSettings[item.key()] = item.value().dump();
            }
          }
        }
      } catch (const std::exception &e) {
        MCO_LOG_WARNING("Failed to parse globalSettings JSON: {}", e.what());
      }
    } else if (lowerKey == "interactivemode") {
      // Parse boolean value (case-insensitive)
      std::string lowerValue = toLower(value);
      if (lowerValue == "true" || lowerValue == "1" || lowerValue == "yes") {
        config.interactiveMode = true;
      } else if (lowerValue == "false" || lowerValue == "0" ||
                 lowerValue == "no") {
        config.interactiveMode = false;
      } else {
        // Invalid value, log warning and use default
        MCO_LOG_WARNING(
            "Invalid interactiveMode value: '{}', using default: false", value);
        config.interactiveMode = false;
      }
    } else if (lowerKey == "quotaname") {
      config.quotaName = value;
    }
  }

  return config;
}

void ConnectionStringParser::updateConfig(const std::string &connStr,
                                          Config &config) {
  for (const auto& [key, value] : parseAttributes(connStr)) {
    const std::string lowerKey = toLower(key);
    if (lowerKey == "endpoint" || lowerKey == "server") {
      config.endpoint = value;
    } else if (lowerKey == "project") {
      config.project = value;
    } else if (lowerKey == "schema") {
      config.schema = value;
    } else if (lowerKey == "accesskeyid" || lowerKey == "uid") {
      config.accessKeyId = value;
    } else if (lowerKey == "accesskeysecret" || lowerKey == "pwd") {
      config.accessKeySecret = value;
    } else if (lowerKey == "securitytoken") {
      config.securityToken = value;
    } else if (lowerKey == "readtimeout") {
      config.readTimeout = std::stoi(value);
    } else if (lowerKey == "connecttimeout") {
      config.connectTimeout = std::stoi(value);
    } else if (lowerKey == "protocol") {
      config.protocol = toLower(value);
      if (config.protocol != "http" && config.protocol != "https") {
        config.protocol = "https";
      }
    } else if (lowerKey == "datasourcename") {
      config.dataSourceName = value;
    } else if (lowerKey == "regionid") {
      config.regionId = value;
    } else if (lowerKey == "useragent") {
      config.userAgent = value;
    } else if (lowerKey == "httpproxy") {
      config.httpProxy = value;
      auto parsed = parseProxyUrl(value);
      if (!std::get<2>(parsed).empty() && !config.proxyUser.has_value()) {
        config.proxyUser = std::get<2>(parsed);
      }
      if (!std::get<3>(parsed).empty() && !config.proxyPassword.has_value()) {
        config.proxyPassword = std::get<3>(parsed);
      }
    } else if (lowerKey == "httpsproxy") {
      config.httpsProxy = value;
      auto parsed = parseProxyUrl(value);
      if (!std::get<2>(parsed).empty() && !config.proxyUser.has_value()) {
        config.proxyUser = std::get<2>(parsed);
      }
      if (!std::get<3>(parsed).empty() && !config.proxyPassword.has_value()) {
        config.proxyPassword = std::get<3>(parsed);
      }
    } else if (lowerKey == "proxyuser") {
      config.proxyUser = value;
    } else if (lowerKey == "proxypassword" || lowerKey == "proxypwd") {
      config.proxyPassword = value;
    } else if (lowerKey == "logpath") {
      maxcompute_odbc::Logger::getInstance().setLogFile(value);
    } else if (lowerKey == "loglevel") {
      maxcompute_odbc::Logger::getInstance().setLogLevel(value);
    } else if (lowerKey == "timezone") {
      config.timezone = value;
    } else if (lowerKey == "namespaceschema") {
      config.namespaceSchema = value != "false";
    } else if (lowerKey == "clientcharset") {
      if (!value.empty()) {
        std::string normalized = trim(value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (normalized == "UTF8") normalized = "UTF-8";
        config.clientCharset = normalized;
        config.clientCharsetSpecified = true;
      }
    } else if (lowerKey == "fetchresultsplitsize") {
      config.fetchResultSplitSize = std::stoll(value);
    } else if (lowerKey == "fetchresultpreloadsplitnum") {
      config.fetchResultPreloadSplitNum = std::stol(value);
    } else if (lowerKey == "fetchresultthreadnum") {
      config.fetchResultThreadNum = std::stol(value);
    } else if (lowerKey == "logviewhost") {
      config.logviewHost = value;
    } else if (lowerKey == "globalsettings") {
      try {
        auto json_obj = nlohmann::json::parse(value);
        if (json_obj.is_object()) {
          for (auto &item : json_obj.items()) {
            if (item.value().is_string()) {
              config.globalSettings[item.key()] =
                  item.value().get<std::string>();
            } else {
              config.globalSettings[item.key()] = item.value().dump();
            }
          }
        }
      } catch (const std::exception &e) {
        MCO_LOG_WARNING("Failed to parse globalSettings JSON: {}", e.what());
      }
    } else if (lowerKey == "interactivemode") {
      // Parse boolean value (case-insensitive)
      std::string lowerValue = toLower(value);
      if (lowerValue == "true" || lowerValue == "1" || lowerValue == "yes") {
        config.interactiveMode = true;
      } else if (lowerValue == "false" || lowerValue == "0" ||
                 lowerValue == "no") {
        config.interactiveMode = false;
      } else {
        // Invalid value, log warning and use default
        MCO_LOG_WARNING(
            "Invalid interactiveMode value: '{}', using default: false", value);
        config.interactiveMode = false;
      }
    } else if (lowerKey == "quotaname") {
      config.quotaName = value;
    }
  }
}

}  // namespace maxcompute_odbc
