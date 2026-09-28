#pragma once
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define NOMINMAX
#endif

#include "maxcompute_odbc/maxcompute_client/models.h"  // For ColumnData
#include <cstdint>
#include <sql.h>
#include <sqlext.h>
#include <vector>
#include "handles.h"  // For ColumnBinding

namespace maxcompute_odbc {

// 将ColumnData中的一个值，根据绑定信息，转换并写入用户缓冲区
// client_charset: SQL_C_CHAR 输出编码 (Config::clientCharset, 默认 "UTF-8").
// 仅影响字符列的 ANSI 输出路径; SQL_C_WCHAR 始终用 UTF-16, 与该参数无关.
SQLRETURN convertAndWrite(const ColumnData &data,
                          const StmtHandle::ColumnBinding &binding,
                          const std::string &client_charset = "UTF-8");

// 把列值转成字符目标 (SQL_C_CHAR / SQL_C_WCHAR) 应当看到的文本.
// 绑定写入 (SQLFetch) 和 SQLGetData 的分段读取共用这一个实现.
std::string ColumnDataToText(const ColumnData &data);

// 构造字符/二进制目标的完整负载 (不含终止符) 和描述"能不能切、怎么切"的
// layout. 非字符且非二进制的 target_type, 或二进制目标拿到非字节值时返回错误,
// 调用方据此退回 convertAndWrite 的定长转换路径.
Result<void> BuildCharacterPayload(const ColumnData &data,
                                   SQLSMALLINT target_type,
                                   const std::string &client_charset,
                                   std::vector<std::uint8_t> &payload,
                                   fetch::Layout &layout);

struct OdbcColumn {
  std::string name;  // 列名 (用于 SQLDescribeCol 的 ColumnName)

  // --- 核心类型信息 ---
  SQLSMALLINT sql_type;  // ODBC SQL类型码, e.g., SQL_VARCHAR, SQL_BIGINT
  int column_size;       // 列的大小 (e.g., VARCHAR(255) -> 255)
  SQLSMALLINT decimal_digits;  // 小数位数 (e.g., DECIMAL(10, 2) -> 2)
  SQLSMALLINT
  nullable;  // 是否可空 (SQL_NULLABLE = 1, SQL_NO_NULLS = 0, UNKNOWN = 2)
  SQLLEN
  octet_length;  // 列的字节长度 - 最大字节数以存储列数据 (for
                 // SQL_DESC_OCTET_LENGTH)
};

Result<std::unique_ptr<OdbcColumn>> convertColumn(const Column &column);

// 将 MaxCompute Table 类转换了 ODBC 标准 ResultSet, 用于 SQLColumns
// 不支持通配符
// @see
// https://learn.microsoft.com/zh-cn/sql/odbc/reference/syntax/sqlcolumns-function?view=sql-server-ver17
Result<std::unique_ptr<ResultStream>> convertTable(
    const Table &table, const std::string &column_pattern);

// 将 MaxCompute Table 列表转换了 ODBC 标准 ResultSet, 用于 SQLTables
// @see
// https://learn.microsoft.com/zh-cn/sql/odbc/reference/syntax/sqltables-function?view=sql-server-ver17
Result<std::unique_ptr<ResultStream>> convertTables(
    const std::vector<TableId> &tables);
}  // namespace maxcompute_odbc
