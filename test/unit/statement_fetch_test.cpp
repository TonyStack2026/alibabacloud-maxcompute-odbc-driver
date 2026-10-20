#include "maxcompute_odbc/common/logging.h"
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
