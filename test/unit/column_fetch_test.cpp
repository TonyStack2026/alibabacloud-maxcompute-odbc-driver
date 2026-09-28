// Unit tests for the SQLGetData segmentation contract. These exercise
// fetch::CopySegment directly: no statement handle, no Driver Manager, no
// MaxCompute connection. The end-to-end side of the same contract (return
// codes as seen through the Driver Manager, SQLGetDiagRec text, statement
// reuse) is covered by test/e2e/fetch_contract.c.
#include "maxcompute_odbc/odbc_api/column_fetch.h"
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using maxcompute_odbc::fetch::CopySegment;
using maxcompute_odbc::fetch::Cursor;
using maxcompute_odbc::fetch::Dbcs;
using maxcompute_odbc::fetch::Layout;
using maxcompute_odbc::fetch::Outcome;

namespace {

std::vector<std::uint8_t> Bytes(const std::string &s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string ToString(const std::vector<std::uint8_t> &v) {
  return std::string(v.begin(), v.end());
}

// SQL_C_CHAR over a non-UTF-8-aware 8-bit payload: cut anywhere, one byte
// per character.
Layout Char() {
  Layout l;
  l.element_bytes = 1;
  l.nul_terminate = true;
  return l;
}

Layout Utf8() {
  Layout l = Char();
  l.utf8_boundary = true;
  return l;
}

Layout Binary() {
  Layout l;
  l.element_bytes = 1;
  l.nul_terminate = false;
  return l;
}

Layout Wide() {
  Layout l;
  l.element_bytes = static_cast<std::uint8_t>(sizeof(SQLWCHAR));
  l.nul_terminate = true;
  l.utf16_boundary = true;
  return l;
}

Layout Gbk() {
  Layout l = Char();
  l.dbcs = Dbcs::Gbk;
  return l;
}

// UTF-16 payload built from code units, native endian.
std::vector<std::uint8_t> Utf16(const std::vector<std::uint16_t> &units) {
  std::vector<std::uint8_t> out(units.size() * sizeof(SQLWCHAR));
  if (!units.empty()) std::memcpy(out.data(), units.data(), out.size());
  return out;
}

// Reads `payload` with a buffer of `step` bytes until the call is no longer
// SQL_SUCCESS_WITH_INFO, concatenating what came back. Returns the segments'
// return codes and indicators so a test can assert on the whole sequence.
struct Reassembly {
  std::string bytes;
  std::vector<SQLRETURN> codes;
  std::vector<SQLLEN> indicators;
  std::size_t calls = 0;
};

Reassembly ReadInParts(const std::vector<std::uint8_t> &payload,
                       const Layout &layout, SQLLEN step,
                       SQLUSMALLINT column = 1) {
  Reassembly r;
  Cursor cursor;
  std::vector<char> buf(static_cast<size_t>(step) + 1, 0x7F);
  SQLLEN indicator = -12345;
  SQLRETURN ret = SQL_SUCCESS_WITH_INFO;
  do {
    Outcome o = CopySegment(payload, layout, cursor, column, buf.data(), step);
    r.codes.push_back(o.ret);
    r.indicators.push_back(o.indicator);
    r.bytes.append(buf.data(), o.copied);
    ++r.calls;
    ret = o.ret;
    indicator = o.indicator;
    if (o.ret == SQL_SUCCESS_WITH_INFO && o.copied == 0) {
      ADD_FAILURE() << "call made no progress: an application looping on this "
                       "column would never terminate";
      break;
    }
    if (r.calls > 20000) {
      ADD_FAILURE() << "segmentation did not terminate";
      break;
    }
  } while (ret == SQL_SUCCESS_WITH_INFO);
  (void)indicator;
  return r;
}

}  // namespace

TEST(ColumnFetchTest, ValueFitsInOneCall) {
  const auto payload = Bytes("hello");
  Cursor cursor;
  char buf[8];
  std::memset(buf, 0x7F, sizeof(buf));

  Outcome o = CopySegment(payload, Char(), cursor, 1, buf, sizeof(buf));
  EXPECT_EQ(o.ret, SQL_SUCCESS);
  EXPECT_FALSE(o.truncated);
  EXPECT_EQ(o.copied, 5u);
  EXPECT_EQ(o.indicator, 5);
  EXPECT_STREQ(buf, "hello");

  // A call after the whole value has been returned reports SQL_NO_DATA.
  Outcome again = CopySegment(payload, Char(), cursor, 1, buf, sizeof(buf));
  EXPECT_EQ(again.ret, SQL_NO_DATA);
  EXPECT_EQ(again.indicator, 0);
}

TEST(ColumnFetchTest, LastByteHeldForTerminatorThenDelivered) {
  // "abc" in a 3-byte buffer: only 2 bytes plus NUL fit, so the first call is
  // a truncation warning and the second delivers the rest.
  const auto payload = Bytes("abc");
  Cursor cursor;
  char buf[3] = {};

  Outcome first = CopySegment(payload, Char(), cursor, 1, buf, 3);
  EXPECT_EQ(first.ret, SQL_SUCCESS_WITH_INFO);
  EXPECT_TRUE(first.truncated);
  EXPECT_EQ(first.copied, 2u);
  EXPECT_EQ(first.indicator, 3);  // length available at the start of the call
  EXPECT_STREQ(buf, "ab");

  Outcome second = CopySegment(payload, Char(), cursor, 1, buf, 3);
  EXPECT_EQ(second.ret, SQL_SUCCESS);  // last part: no warning, real length
  EXPECT_EQ(second.copied, 1u);
  EXPECT_EQ(second.indicator, 1);
  EXPECT_STREQ(buf, "c");
}

TEST(ColumnFetchTest, IndicatorDecreasesAcrossParts) {
  const auto payload = Bytes("abcdefghij");  // 10 bytes
  const Reassembly r = ReadInParts(payload, Char(), 4);
  // A 4-byte buffer carries 3 payload bytes plus the terminator, so 10 bytes
  // take four calls and the reported length walks down 10, 7, 4, 1.
  ASSERT_EQ(r.codes.size(), 4u);
  EXPECT_EQ(r.codes[0], SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(r.codes[1], SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(r.codes[2], SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(r.codes[3], SQL_SUCCESS);
  EXPECT_EQ(r.indicators[0], 10);
  EXPECT_EQ(r.indicators[1], 7);
  EXPECT_EQ(r.indicators[2], 4);
  EXPECT_EQ(r.indicators[3], 1);
  EXPECT_EQ(r.bytes, "abcdefghij");
}

TEST(ColumnFetchTest, LengthOnlyCallReportsLengthAndConsumesNothing) {
  const auto payload = Bytes("abcdef");
  Cursor cursor;

  Outcome probe = CopySegment(payload, Char(), cursor, 1, nullptr, 0);
  EXPECT_EQ(probe.ret, SQL_SUCCESS_WITH_INFO);
  EXPECT_TRUE(probe.truncated);
  EXPECT_EQ(probe.indicator, 6);
  EXPECT_EQ(probe.copied, 0u);

  char buf[7] = {};
  Outcome read = CopySegment(payload, Char(), cursor, 1, buf, sizeof(buf));
  EXPECT_EQ(read.ret, SQL_SUCCESS);
  EXPECT_STREQ(buf, "abcdef");
}

TEST(ColumnFetchTest, EmptyStringIsSuccessWithZeroLength) {
  const std::vector<std::uint8_t> payload;
  Cursor cursor;
  char buf[4];
  std::memset(buf, 0x7F, sizeof(buf));

  Outcome o = CopySegment(payload, Char(), cursor, 1, buf, sizeof(buf));
  EXPECT_EQ(o.ret, SQL_SUCCESS);
  EXPECT_FALSE(o.truncated);
  EXPECT_EQ(o.copied, 0u);
  EXPECT_EQ(o.indicator, 0);
  EXPECT_EQ(buf[0], '\0');

  EXPECT_EQ(CopySegment(payload, Char(), cursor, 1, buf, sizeof(buf)).ret,
            SQL_NO_DATA);
}

TEST(ColumnFetchTest, Utf8CharacterIsNotSplitWhenItCanBeAvoided) {
  // "你好" is 6 UTF-8 bytes. A 5-byte buffer has room for 4 payload bytes, but
  // the second character starts at 3, so the cut must land at 3.
  const auto payload = Bytes("\xE4\xBD\xA0\xE5\xA5\xBD");
  Cursor cursor;
  char buf[5] = {};

  Outcome first = CopySegment(payload, Utf8(), cursor, 1, buf, 5);
  EXPECT_EQ(first.ret, SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(first.copied, 3u);
  EXPECT_EQ(first.indicator, 6);
  EXPECT_EQ(buf[3], '\0');

  Outcome second = CopySegment(payload, Utf8(), cursor, 1, buf, 5);
  EXPECT_EQ(second.ret, SQL_SUCCESS);
  EXPECT_EQ(second.copied, 3u);
  EXPECT_EQ(second.indicator, 3);
}

TEST(ColumnFetchTest, Utf8CharacterLongerThanBufferStillMakesProgress) {
  // "你" (3 bytes) into a 2-byte buffer: one payload byte plus NUL at most.
  // Returning nothing would let an application loop forever, so the character
  // is split; reassembly stays byte-exact.
  const auto payload = Bytes("\xE4\xBD\xA0");
  const Reassembly r = ReadInParts(payload, Utf8(), 2);
  EXPECT_EQ(r.bytes, ToString(payload));
  ASSERT_EQ(r.codes.size(), 3u);
  EXPECT_EQ(r.codes.back(), SQL_SUCCESS);
  EXPECT_EQ(r.indicators[0], 3);
  EXPECT_EQ(r.indicators[1], 2);
  EXPECT_EQ(r.indicators[2], 1);
}

TEST(ColumnFetchTest, GbkCutStaysOnTwoByteCharacterWhenPossible) {
  // "你好" in GBK is C4 E3 BA C3.
  const auto payload = Bytes("\xC4\xE3\xBA\xC3");
  const Reassembly r = ReadInParts(payload, Gbk(), 3);
  ASSERT_EQ(r.codes.size(), 2u);
  EXPECT_EQ(r.codes[0], SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(r.indicators[0], 4);
  EXPECT_EQ(r.indicators[1], 2);
  // Two whole characters, never a lead byte alone.
  EXPECT_EQ(r.bytes, ToString(payload));
}

TEST(ColumnFetchTest, GbkLeadByteIsStillHandedOverWhenBufferIsSmaller) {
  // A 2-byte buffer leaves room for one payload byte plus NUL. Splitting the
  // character is the only way to make progress; the parts still reassemble to
  // exactly the original bytes.
  const auto payload = Bytes("\xC4\xE3\xBA\xC3");
  const Reassembly r = ReadInParts(payload, Gbk(), 2);
  EXPECT_EQ(r.bytes, ToString(payload));
  EXPECT_EQ(r.codes.back(), SQL_SUCCESS);
}

TEST(ColumnFetchTest, BinarySegmentsHaveNoTerminatorAndNoSnapping) {
  const auto payload = Bytes(std::string("\x00\x01\x02\x03\x04\x05", 6));
  Cursor cursor;
  char buf[3] = {'x', 'x', 'x'};

  Outcome first = CopySegment(payload, Binary(), cursor, 1, buf, 3);
  EXPECT_EQ(first.ret, SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(first.copied, 3u);
  EXPECT_EQ(first.indicator, 6);
  EXPECT_EQ(buf[0], '\x00');
  EXPECT_EQ(buf[1], '\x01');
  EXPECT_EQ(buf[2], '\x02');  // untouched by any terminator write

  Outcome second = CopySegment(payload, Binary(), cursor, 1, buf, 3);
  EXPECT_EQ(second.ret, SQL_SUCCESS);
  EXPECT_EQ(second.copied, 3u);
  EXPECT_EQ(second.indicator, 3);
}

TEST(ColumnFetchTest, WideSegmentsKeepCodeUnitsAndSurrogatePairsTogether) {
  // U+1F600 (4-byte UTF-8, one surrogate pair) followed by 'A'.
  const auto payload = Utf16({0xD83D, 0xDE00, 'A'});
  Cursor cursor;
  std::vector<char> buf(6, 0x7F);  // room for 2 units plus the terminator

  Outcome first = CopySegment(payload, Wide(), cursor, 1, buf.data(), 6);
  EXPECT_EQ(first.ret, SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(first.copied, 4u);  // both halves of the pair, never just one
  EXPECT_EQ(first.indicator, 6);
  SQLWCHAR terminated = 0x7F7F;
  std::memcpy(&terminated, buf.data() + 4, sizeof(terminated));
  EXPECT_EQ(terminated, 0);

  Outcome second = CopySegment(payload, Wide(), cursor, 1, buf.data(), 6);
  EXPECT_EQ(second.ret, SQL_SUCCESS);
  EXPECT_EQ(second.copied, 2u);
  EXPECT_EQ(second.indicator, 2);
}

TEST(ColumnFetchTest, WideAstralCharacterSplitWhenItCannotFitAtAll) {
  // Only one code unit of room per call: progress beats keeping the pair
  // together, and the two halves still reassemble in order.
  const auto payload = Utf16({0xD83D, 0xDE00});
  const Reassembly r = ReadInParts(payload, Wide(), 4);
  ASSERT_EQ(r.codes.size(), 2u);
  EXPECT_EQ(r.codes[0], SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(r.codes[1], SQL_SUCCESS);
  EXPECT_EQ(r.bytes, ToString(payload));
}

TEST(ColumnFetchTest, ChangingColumnRestartsTheOffset) {
  // "Successive calls to SQLGetData will retrieve data from the last column
  // requested; prior offsets become invalid."
  const auto col1 = Bytes("abcdef");
  const auto col2 = Bytes("xyz");
  Cursor cursor;
  char buf[4] = {};

  Outcome a = CopySegment(col1, Char(), cursor, 1, buf, 4);
  EXPECT_EQ(a.ret, SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(a.indicator, 6);

  Outcome b = CopySegment(col2, Char(), cursor, 2, buf, 4);
  EXPECT_EQ(b.ret, SQL_SUCCESS);  // "xyz" plus NUL fits exactly in 4 bytes
  EXPECT_EQ(b.indicator, 3);

  // Back to column 1: this is a fresh read of the whole value, not the tail.
  Outcome a2 = CopySegment(col1, Char(), cursor, 1, buf, 4);
  EXPECT_EQ(a2.ret, SQL_SUCCESS_WITH_INFO);
  EXPECT_EQ(a2.indicator, 6);
  EXPECT_EQ(a2.copied, 3u);
  EXPECT_STREQ(buf, "abc");
}

TEST(ColumnFetchTest, LongValueRoundTripsThroughEveryBufferSize) {
  // 300 bytes is deliberately not a round number of segments for any of these.
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text.push_back(static_cast<char>('a' + (i % 26)));
  }
  const auto payload = Bytes(text);
  // step is the BufferLength: the usable payload space is one byte less, so
  // the sequence of segment sizes changes with each value. step 1 (no room for
  // a character at all) is covered by OneByteCharBufferCannotReturnData.
  for (SQLLEN step : {2, 3, 5, 7, 16, 100, 299, 300, 301, 1024}) {
    const Reassembly r = ReadInParts(payload, Char(), step);
    EXPECT_EQ(r.bytes, text) << "step=" << step;
    EXPECT_EQ(r.codes.back(), SQL_SUCCESS) << "step=" << step;
  }
}

TEST(ColumnFetchTest, OneByteCharBufferCannotReturnDataButDoesNotConsume) {
  // BufferLength 1 leaves no room for a character plus the terminator. The
  // documented answer is "nothing copied, still truncated, here is the
  // length": a caller that loops on this without growing the buffer is the
  // caller's bug, but the driver must not write past one byte or pretend the
  // value was delivered.
  const auto payload = Bytes("abcdef");
  Cursor cursor;
  char buf[1] = {'x'};

  for (int i = 0; i < 3; ++i) {
    Outcome o = CopySegment(payload, Char(), cursor, 1, buf, 1);
    EXPECT_EQ(o.ret, SQL_SUCCESS_WITH_INFO);
    EXPECT_TRUE(o.truncated);
    EXPECT_EQ(o.copied, 0u);
    EXPECT_EQ(o.indicator, 6);
    // The only byte is the terminator, so the buffer stays a readable (empty)
    // string instead of holding whatever was in it before.
    EXPECT_EQ(buf[0], '\0');
  }

  // Nothing was consumed: a real buffer now reads the value from the start.
  char whole[7] = {};
  Outcome full = CopySegment(payload, Char(), cursor, 1, whole, sizeof(whole));
  EXPECT_EQ(full.ret, SQL_SUCCESS);
  EXPECT_STREQ(whole, "abcdef");
}

TEST(ColumnFetchTest, Utf8TextRoundTripsThroughEveryBufferSize) {
  std::string text;
  for (int i = 0; i < 40; ++i) {
    text += "\xE4\xBD\xA0";      // 3-byte
    text += "a";                 // 1-byte
    text += "\xF0\x9F\x98\x80";  // 4-byte
  }
  const auto payload = Bytes(text);
  for (SQLLEN step : {SQLLEN(2), SQLLEN(3), SQLLEN(4), SQLLEN(5), SQLLEN(6),
                      SQLLEN(9), SQLLEN(13), SQLLEN(64), SQLLEN(200),
                      static_cast<SQLLEN>(text.size())}) {
    const Reassembly r = ReadInParts(payload, Utf8(), step);
    EXPECT_EQ(r.bytes, ToString(payload)) << "step=" << step;
    EXPECT_EQ(r.codes.back(), SQL_SUCCESS) << "step=" << step;
  }
}
