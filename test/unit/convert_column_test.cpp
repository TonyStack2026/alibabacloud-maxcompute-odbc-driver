// Tests for convertColumn — the type-mapping table that decides which ODBC
// SQL type a MaxCompute column is reported as.
//
// Key invariants:
//   - Text types (STRING / VARCHAR / CHAR / JSON) map to Unicode SQL types
//     (SQL_WVARCHAR / SQL_WCHAR), so metadata-driven Wide-API consumers
//     can select SQL_C_WCHAR. Explicit SQL_C_CHAR bindings remain supported.
//   - octet_length for WCHAR types must use sizeof(SQLWCHAR), not a literal
//     2, so the value is correct on both UTF-16 (Windows / unixODBC) and
//     UTF-32 (iODBC) hosts.
//   - BINARY → SQL_VARBINARY so the DM never charset-converts raw bytes.
//   - DECIMAL stays SQL_VARCHAR (unchanged): callers consume ASCII digits.

#include "maxcompute_odbc/maxcompute_client/models.h"
#include "maxcompute_odbc/maxcompute_client/typeinfo.h"
#include "maxcompute_odbc/odbc_api/conversions.h"
#include <gtest/gtest.h>
#include <memory>
#include <sql.h>
#include <sqlext.h>

using maxcompute_odbc::Column;
using maxcompute_odbc::convertColumn;

namespace {

Column MakeColumn(const std::string &name, std::unique_ptr<typeinfo> ti) {
  return Column::buildColumn(name, std::move(ti));
}

}  // namespace

TEST(ConvertColumnTest, StringMapsToWVarchar) {
  Column col =
      MakeColumn("s", std::make_unique<PrimitiveTypeInfo>(OdpsType::STRING));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  const auto &out = *result.value();
  EXPECT_EQ(out.sql_type, SQL_WVARCHAR);
  EXPECT_EQ(out.column_size, 65535);
  EXPECT_EQ(static_cast<size_t>(out.octet_length),
            static_cast<size_t>(65535) * sizeof(SQLWCHAR));
  EXPECT_EQ(out.name, "s");
}

TEST(ConvertColumnTest, JsonMapsToWVarchar) {
  Column col =
      MakeColumn("j", std::make_unique<PrimitiveTypeInfo>(OdpsType::JSON));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  const auto &out = *result.value();
  EXPECT_EQ(out.sql_type, SQL_WVARCHAR);
  EXPECT_EQ(out.column_size, 65535);
  EXPECT_EQ(static_cast<size_t>(out.octet_length),
            static_cast<size_t>(65535) * sizeof(SQLWCHAR));
}

TEST(ConvertColumnTest, Varchar50MapsToWVarchar) {
  Column col = MakeColumn("v", std::make_unique<VarcharTypeInfo>(50));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  const auto &out = *result.value();
  EXPECT_EQ(out.sql_type, SQL_WVARCHAR);
  EXPECT_EQ(out.column_size, 50);
  EXPECT_EQ(static_cast<size_t>(out.octet_length), 50u * sizeof(SQLWCHAR));
}

TEST(ConvertColumnTest, Char20MapsToWChar) {
  Column col = MakeColumn("c", std::make_unique<CharTypeInfo>(20));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  const auto &out = *result.value();
  EXPECT_EQ(out.sql_type, SQL_WCHAR);
  EXPECT_EQ(out.column_size, 20);
  EXPECT_EQ(static_cast<size_t>(out.octet_length), 20u * sizeof(SQLWCHAR));
}

TEST(ConvertColumnTest, BinaryMapsToVarbinary) {
  Column col =
      MakeColumn("b", std::make_unique<PrimitiveTypeInfo>(OdpsType::BINARY));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  const auto &out = *result.value();
  EXPECT_EQ(out.sql_type, SQL_VARBINARY);
  EXPECT_EQ(out.column_size, 65535);
  EXPECT_EQ(out.octet_length, 65535);
}

TEST(ConvertColumnTest, DecimalUnchangedAsVarchar) {
  // DECIMAL stays SQL_VARCHAR — flipping it is a separate, larger change.
  Column col = MakeColumn("d", std::make_unique<DecimalTypeInfo>(38, 10));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  const auto &out = *result.value();
  EXPECT_EQ(out.sql_type, SQL_VARCHAR);
}

TEST(ConvertColumnTest, ArrayOfStringMapsToWVarchar) {
  auto inner = std::make_unique<PrimitiveTypeInfo>(OdpsType::STRING);
  Column col =
      MakeColumn("a", std::make_unique<ArrayTypeInfo>(std::move(inner)));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  const auto &out = *result.value();
  EXPECT_EQ(out.sql_type, SQL_WVARCHAR);
  EXPECT_EQ(out.column_size, 65535);
  EXPECT_EQ(static_cast<size_t>(out.octet_length),
            static_cast<size_t>(65535) * sizeof(SQLWCHAR));
}

TEST(ConvertColumnTest, MapMapsToWVarchar) {
  auto k = std::make_unique<PrimitiveTypeInfo>(OdpsType::STRING);
  auto v = std::make_unique<PrimitiveTypeInfo>(OdpsType::BIGINT);
  Column col = MakeColumn(
      "m", std::make_unique<MapTypeInfo>(std::move(k), std::move(v)));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->sql_type, SQL_WVARCHAR);
}

TEST(ConvertColumnTest, IntegerTypesMapToBigint) {
  for (OdpsType t : {OdpsType::TINYINT, OdpsType::SMALLINT, OdpsType::INT,
                     OdpsType::BIGINT}) {
    Column col = MakeColumn("i", std::make_unique<PrimitiveTypeInfo>(t));
    auto result = convertColumn(col);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->sql_type, SQL_BIGINT)
        << "type=" << static_cast<int>(t);
  }
}

TEST(ConvertColumnTest, FloatAndDoubleMapToDouble) {
  for (OdpsType t : {OdpsType::FLOAT, OdpsType::DOUBLE}) {
    Column col = MakeColumn("f", std::make_unique<PrimitiveTypeInfo>(t));
    auto result = convertColumn(col);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->sql_type, SQL_DOUBLE)
        << "type=" << static_cast<int>(t);
  }
}

TEST(ConvertColumnTest, BooleanMapsToBit) {
  Column col =
      MakeColumn("b", std::make_unique<PrimitiveTypeInfo>(OdpsType::BOOLEAN));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->sql_type, SQL_BIT);
}

TEST(ConvertColumnTest, DateMapsToTypeDate) {
  Column col =
      MakeColumn("d", std::make_unique<PrimitiveTypeInfo>(OdpsType::DATE));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->sql_type, SQL_TYPE_DATE);
}

TEST(ConvertColumnTest, TimestampMapsToTypeTimestamp) {
  Column col = MakeColumn(
      "ts", std::make_unique<PrimitiveTypeInfo>(OdpsType::TIMESTAMP));
  auto result = convertColumn(col);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->sql_type, SQL_TYPE_TIMESTAMP);
  EXPECT_EQ(result.value()->decimal_digits, 9);
}
