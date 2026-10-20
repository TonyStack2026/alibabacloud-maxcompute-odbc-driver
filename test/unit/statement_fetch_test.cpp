#include "maxcompute_odbc/common/logging.h"
#include "maxcompute_odbc/maxcompute_client/typeinfo.h"
#include "maxcompute_odbc/odbc_api/handles.h"
#include <cstring>
#include <gtest/gtest.h>

using namespace maxcompute_odbc;

TEST(StatementFetch, NullColumnInvalidatesPreviousReadPosition) {
  for (const SQLLEN first_buffer_size : {SQLLEN(4), SQLLEN(16)}) {
    StmtHandle stmt(nullptr);
    std::vector<Column> columns(2);
    auto schema = std::make_shared<ResultSetSchema>(std::move(columns));
    Record row;
    row.values.emplace_back(std::string("abcdef"));
    row.values.emplace_back(std::monostate{});
    std::vector<Record> rows;
    rows.push_back(std::move(row));
    stmt.setResultStream(
        std::make_unique<StaticResultStream>(schema, std::move(rows)));
    ASSERT_EQ(SQL_SUCCESS, stmt.fetch());
    char buffer[16] = {};
    SQLLEN indicator = 0;
    ASSERT_TRUE(SQL_SUCCEEDED(
        stmt.getData(1, SQL_C_CHAR, buffer, first_buffer_size, &indicator)));
    ASSERT_EQ(SQL_SUCCESS,
              stmt.getData(2, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
    ASSERT_EQ(SQL_NULL_DATA, indicator);
    ASSERT_EQ(SQL_SUCCESS,
              stmt.getData(1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
    EXPECT_EQ(6, indicator);
    EXPECT_STREQ("abcdef", buffer);
  }
}

TEST(StatementFetch, ParsedCharsetReachesBoundAndGetDataResults) {
  const std::string utf8 = "\xe4\xbd\xa0\xe5\xa5\xbd";
  for (const char* input :
       {"Project=\"p\";ClientCharset=GBK;", "Project=p;ClientCharset={GBK};"}) {
    for (const SQLSMALLINT type :
         {SQLSMALLINT(SQL_C_CHAR), SQLSMALLINT(SQL_C_WCHAR)}) {
      for (const bool bound : {false, true}) {
        SCOPED_TRACE(input);
        SCOPED_TRACE(type);
        SCOPED_TRACE(bound);
        ConnHandle connection(nullptr);
        ConnectionStringParser::updateConfig(input,
                                             connection.getConfigForUpdate());
        StmtHandle stmt(&connection);
        std::vector<Column> columns(1);
        auto schema = std::make_shared<ResultSetSchema>(std::move(columns));
        std::vector<Record> rows(2);
        for (auto& row : rows) row.values.emplace_back(utf8);
        stmt.setResultStream(
            std::make_unique<StaticResultStream>(schema, std::move(rows)));
        SQLWCHAR buffer[32] = {};
        SQLLEN indicator = 0;
        if (bound)
          ASSERT_EQ(SQL_SUCCESS,
                    stmt.bindCol(1, type, buffer, sizeof(buffer), &indicator));
        Logger::getInstance().setLogLevel("INFO");
        testing::internal::CaptureStdout();
        for (int row = 0; row < 2; ++row) {
          EXPECT_EQ(SQL_SUCCESS, stmt.fetch());
          if (!bound)
            EXPECT_EQ(SQL_SUCCESS, stmt.getData(1, type, buffer, sizeof(buffer),
                                                &indicator));
          EXPECT_EQ(4, indicator);
          if (type == SQL_C_CHAR) {
            EXPECT_EQ(0, std::memcmp(buffer, "\xc4\xe3\xba\xc3", 4));
          } else {
            EXPECT_EQ(0x4f60, buffer[0]);
            EXPECT_EQ(0x597d, buffer[1]);
          }
        }
        const auto diagnostic = testing::internal::GetCapturedStdout();
        const auto first = diagnostic.find("[ODBC-Encoding]");
        ASSERT_NE(std::string::npos, first);
        EXPECT_EQ(std::string::npos,
                  diagnostic.find("[ODBC-Encoding]", first + 1));
        EXPECT_EQ(std::string::npos, diagnostic.find(utf8));
        EXPECT_NE(std::string::npos,
                  diagnostic.find(type == SQL_C_CHAR ? "encoding=GBK"
                                                     : "encoding=UTF-16"));
      }
    }
  }
}

TEST(StatementMetadata, TextColumnsReportUnicodeTypesAndOdbcTypeNames) {
  StmtHandle stmt(nullptr);
  std::vector<Column> columns;
  columns.push_back(
      Column::buildColumn("i", PrimitiveTypeInfo(OdpsType::BIGINT)));
  columns.push_back(
      Column::buildColumn("s", PrimitiveTypeInfo(OdpsType::STRING)));
  columns.push_back(Column::buildColumn("v", VarcharTypeInfo(50)));
  columns.push_back(Column::buildColumn("c", CharTypeInfo(20)));
  columns.push_back(
      Column::buildColumn("b", PrimitiveTypeInfo(OdpsType::BINARY)));
  columns.push_back(Column::buildColumn("d", DecimalTypeInfo(38, 10)));
  auto schema = std::make_shared<ResultSetSchema>(std::move(columns));
  stmt.setResultStream(
      std::make_unique<StaticResultStream>(schema, std::vector<Record>{}));

  struct Expected {
    SQLUSMALLINT column;
    SQLSMALLINT type;
    SQLULEN size;
    const char* type_name;
  };
  const Expected expected[] = {
      {1, SQL_BIGINT, 19, "BIGINT"},
      {2, SQL_WVARCHAR, 65535, "WVARCHAR"},
      {3, SQL_WVARCHAR, 50, "WVARCHAR"},
      {4, SQL_WCHAR, 20, "WCHAR"},
      {5, SQL_VARBINARY, 65535, "VARBINARY"},
      {6, SQL_VARCHAR, 65535, "VARCHAR"},
  };

  for (const auto& e : expected) {
    SQLCHAR name[64] = {};
    SQLSMALLINT name_len = 0;
    SQLSMALLINT type = 0;
    SQLULEN size = 0;
    SQLSMALLINT decimal_digits = 0;
    SQLSMALLINT nullable = 0;
    ASSERT_EQ(SQL_SUCCESS,
              stmt.describeCol(e.column, name, sizeof(name), &name_len, &type,
                               &size, &decimal_digits, &nullable))
        << "column " << e.column;
    EXPECT_EQ(e.type, type) << "column " << e.column;
    EXPECT_EQ(e.size, size) << "column " << e.column;

    char narrow[32] = {};
    SQLSMALLINT narrow_len = -1;
    ASSERT_EQ(SQL_SUCCESS,
              stmt.getColAttribute(e.column, SQL_DESC_TYPE_NAME, narrow,
                                   sizeof(narrow), &narrow_len, nullptr));
    EXPECT_STREQ(e.type_name, narrow) << "column " << e.column;
    EXPECT_EQ(static_cast<SQLSMALLINT>(std::strlen(e.type_name)), narrow_len);

    SQLWCHAR wide[32] = {};
    SQLSMALLINT wide_len = -1;
    ASSERT_EQ(SQL_SUCCESS,
              stmt.getColAttribute(e.column, SQL_DESC_TYPE_NAME, wide,
                                   sizeof(wide), &wide_len, nullptr, true));
    EXPECT_EQ(static_cast<SQLSMALLINT>(std::strlen(e.type_name)), wide_len);
    for (size_t i = 0; e.type_name[i] != '\0'; ++i) {
      EXPECT_EQ(static_cast<SQLWCHAR>(e.type_name[i]), wide[i]);
    }
    EXPECT_EQ(0, wide[std::strlen(e.type_name)]);

    SQLSMALLINT length_only = -1;
    ASSERT_EQ(SQL_SUCCESS,
              stmt.getColAttribute(e.column, SQL_DESC_TYPE_NAME, nullptr, 0,
                                   &length_only, nullptr));
    EXPECT_EQ(static_cast<SQLSMALLINT>(std::strlen(e.type_name)), length_only);
  }
}

TEST(StatementMetadata, WideTypeNameRespectsByteBufferLength) {
  StmtHandle stmt(nullptr);
  std::vector<Column> columns;
  columns.push_back(
      Column::buildColumn("s", PrimitiveTypeInfo(OdpsType::STRING)));
  auto schema = std::make_shared<ResultSetSchema>(std::move(columns));
  stmt.setResultStream(
      std::make_unique<StaticResultStream>(schema, std::vector<Record>{}));

  // 6 bytes hold three UTF-16 units: two characters plus the terminator.
  SQLWCHAR buffer[4] = {0x5555, 0x5555, 0x5555, 0x5555};
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS, stmt.getColAttribute(1, SQL_DESC_TYPE_NAME, buffer, 6,
                                              &length, nullptr, true));
  EXPECT_EQ(8, length);  // Full "WVARCHAR", even though the buffer truncated.
  EXPECT_EQ('W', buffer[0]);
  EXPECT_EQ('V', buffer[1]);
  EXPECT_EQ(0, buffer[2]);
  EXPECT_EQ(0x5555, buffer[3]);
}
