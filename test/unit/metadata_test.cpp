#include "maxcompute_odbc/odbc_api/entry_points.h"
#include <gtest/gtest.h>

class Metadata : public testing::Test {
 protected:
  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &env));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc));
  }
  void TearDown() override {
    if (dbc) SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    if (env) SQLFreeHandle(SQL_HANDLE_ENV, env);
  }
};

TEST_F(Metadata, DriverVersionUsesUtf16AndByteLengths) {
  SQLWCHAR output[16] = {};
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetInfoW(dbc, SQL_DRIVER_VER, output, sizeof(output), &length));
  EXPECT_EQ(10, length);
  const SQLWCHAR expected[] = {'1', '.', '0', '.', '1', 0};
  for (size_t i = 0; i < 6; ++i) EXPECT_EQ(expected[i], output[i]);
  char narrow[16] = {};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetInfo(dbc, SQL_DRIVER_VER, narrow, sizeof(narrow), &length));
  EXPECT_STREQ("1.0.1", narrow);
  EXPECT_EQ(5, length);
}

TEST_F(Metadata, ExportsUnicodeConnectionEntryPoint) {
  EXPECT_EQ(SQL_INVALID_HANDLE,
            SQLConnectW(nullptr, nullptr, 0, nullptr, 0, nullptr, 0));
}

TEST_F(Metadata, ReportsImplementedFunctionsInBothCapabilityArrays) {
  SQLUSMALLINT supported = SQL_FALSE;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(dbc, SQL_API_SQLDRIVERCONNECT, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, SQL_API_SQLENDTRAN, &supported));
  EXPECT_EQ(SQL_FALSE, supported);
  SQLUSMALLINT legacy[100] = {};
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, SQL_API_ALL_FUNCTIONS, legacy));
  EXPECT_EQ(SQL_TRUE, legacy[SQL_API_SQLCONNECT]);
  EXPECT_EQ(SQL_TRUE, legacy[SQL_API_SQLGETFUNCTIONS]);
  SQLUSMALLINT odbc3[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE] = {};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(dbc, SQL_API_ODBC3_ALL_FUNCTIONS, odbc3));
  EXPECT_TRUE(SQL_FUNC_EXISTS(odbc3, SQL_API_SQLDRIVERCONNECT));
  EXPECT_TRUE(SQL_FUNC_EXISTS(odbc3, SQL_API_SQLGETCONNECTATTR));
  EXPECT_FALSE(SQL_FUNC_EXISTS(odbc3, SQL_API_SQLENDTRAN));
}

TEST_F(Metadata, ExportsConnectionAttributeGetters) {
  SQLUINTEGER autocommit = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttrW(dbc, SQL_ATTR_AUTOCOMMIT, &autocommit,
                               sizeof(autocommit), nullptr));
  EXPECT_EQ(SQL_AUTOCOMMIT_ON, autocommit);
}

TEST_F(Metadata, ExportsUnicodeStatementAttributeGetter) {
  SQLHSTMT statement = SQL_NULL_HSTMT;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &statement));
  SQLULEN cursor_type = 0;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetStmtAttrW(statement, SQL_ATTR_CURSOR_TYPE, &cursor_type,
                            sizeof(cursor_type), nullptr));
  EXPECT_EQ(SQL_CURSOR_FORWARD_ONLY, cursor_type);
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
}

TEST_F(Metadata,
       UnsupportedTypeInfoReturnsDiagnosticWithoutAdvertisingSupport) {
  SQLUSMALLINT supported = SQL_TRUE;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(dbc, SQL_API_SQLGETTYPEINFO, &supported));
  EXPECT_EQ(SQL_FALSE, supported);
  SQLHSTMT statement = SQL_NULL_HSTMT;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &statement));
  EXPECT_EQ(SQL_ERROR, SQLGetTypeInfoW(statement, SQL_VARCHAR));
  SQLCHAR state[6] = {};
  SQLCHAR message[128] = {};
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(SQL_HANDLE_STMT, statement, 1, state, nullptr,
                          message, sizeof(message), nullptr));
  EXPECT_STREQ("IM001", reinterpret_cast<char*>(state));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
}

TEST_F(Metadata, SmallBuffersTruncateWithoutWritingOutsideTheirCapacity) {
  for (const SQLSMALLINT capacity :
       {SQLSMALLINT(0), SQLSMALLINT(1), SQLSMALLINT(4)}) {
    SQLWCHAR output[] = {0x5555, 0x5555, 0x5555};
    SQLSMALLINT length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetInfoW(dbc, SQL_DRIVER_VER, output, capacity, &length));
    EXPECT_EQ(10, length);
    if (capacity < 2)
      EXPECT_EQ(0x5555, output[0]);
    else {
      EXPECT_EQ('1', output[0]);
      EXPECT_EQ(0, output[1]);
    }
    EXPECT_EQ(0x5555, output[2]);
  }
  SQLSMALLINT length = -1;
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfoW(dbc, SQL_DRIVER_VER, nullptr, 0, &length));
  EXPECT_EQ(10, length);
}

TEST_F(Metadata, OtherStringsAndNumericAttributesRetainTheirTypes) {
  SQLWCHAR output[512] = {};
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetInfoW(dbc, SQL_DBMS_NAME, output, sizeof(output), &length));
  EXPECT_EQ(20, length);
  EXPECT_EQ('M', output[0]);
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetInfoW(dbc, SQL_KEYWORDS, output, sizeof(output), &length));
  EXPECT_GT(length, 20);
  EXPECT_EQ('S', output[0]);
  SQLUSMALLINT limit = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfoW(dbc, SQL_MAX_TABLE_NAME_LEN, &limit,
                                     sizeof(limit), &length));
  EXPECT_EQ(128, limit);
  EXPECT_EQ(sizeof(limit), length);
}

TEST_F(Metadata, ReportsCharacterConversionCapabilities) {
  const SQLUSMALLINT infos[] = {
      SQL_CONVERT_CHAR,  SQL_CONVERT_VARCHAR,  SQL_CONVERT_LONGVARCHAR,
      SQL_CONVERT_WCHAR, SQL_CONVERT_WVARCHAR, SQL_CONVERT_WLONGVARCHAR};
  for (const SQLUSMALLINT info : infos) {
    SQLUINTEGER caps = 0;
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfoW(dbc, info, &caps, sizeof(caps), &length))
        << "info=" << info;
    EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(caps)), length);
    EXPECT_EQ(SQL_CVT_CHAR | SQL_CVT_VARCHAR | SQL_CVT_WCHAR | SQL_CVT_WVARCHAR,
              caps)
        << "info=" << info;
  }
}
