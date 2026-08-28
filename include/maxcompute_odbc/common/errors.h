#pragma once

#include <string>
#include <type_traits>
#include "tl/expected.hpp"

namespace maxcompute_odbc {
// 定义通用错误码
enum class ErrorCode {
  Success = 0,
  InvalidParameter,
  MemoryAllocationFailed,
  FileOpenFailed,
  NetworkError,
  ParseError,
  TimeoutError,
  ExecutionError,
  DataError,
  InternalError,
  NotImplemented,
  UnknownError,
  ResourceNotFound,
  // Tunnel 下载会话表明该 instance 无可下载的表格结果(如 DDL/SET 等非表格
  // 语句)。区别于网络/超时/服务端异常, 仅此情形可安全降级为单列 raw result。
  ResultNotDownloadable
};

// 错误信息结构
struct ErrorInfo {
  ErrorCode code;
  std::string message;
};

template <typename T>
using Result = tl::expected<T, ErrorInfo>;

template <typename T>
Result<std::remove_cv_t<std::remove_reference_t<T>>> makeSuccess(T &&value) {
  return std::forward<T>(value);
}

inline Result<void> makeSuccess() { return {}; }

template <typename T>
Result<T> makeError(ErrorCode code, const std::string &message) {
  return tl::make_unexpected(ErrorInfo{code, message});
}
}  // namespace maxcompute_odbc
