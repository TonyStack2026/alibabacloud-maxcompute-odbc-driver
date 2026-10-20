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
