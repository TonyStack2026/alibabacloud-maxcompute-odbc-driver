#include "maxcompute_odbc/odbc_api/encoding.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using maxcompute_odbc::encoding::WriteUtf8AsCharset;

namespace {

// "你好" in UTF-8 is E4 BD A0  E5 A5 BD (6 bytes).
// In GBK it is C4 E3  BA C3 (4 bytes).
const std::string kNiHaoUtf8 = "\xE4\xBD\xA0\xE5\xA5\xBD";
const std::string kNiHaoGbk = "\xC4\xE3\xBA\xC3";

}  // namespace

TEST(EncodingTest, Utf8PassThrough) {
  char buf[16] = {};
  size_t total = WriteUtf8AsCharset("hello", "UTF-8", buf, sizeof(buf));
  EXPECT_EQ(total, 5u);
  EXPECT_STREQ(buf, "hello");
}

TEST(EncodingTest, EmptyCharsetTreatedAsUtf8) {
  char buf[16] = {};
  size_t total = WriteUtf8AsCharset(kNiHaoUtf8, "", buf, sizeof(buf));
  EXPECT_EQ(total, kNiHaoUtf8.size());
  EXPECT_EQ(0, std::memcmp(buf, kNiHaoUtf8.data(), kNiHaoUtf8.size()));
  EXPECT_EQ('\0', buf[kNiHaoUtf8.size()]);
}

TEST(EncodingTest, Utf8ByteBoundaryTruncation) {
  // "你好" is 6 bytes UTF-8. Buffer can hold 5 bytes (4 chars + NUL).
  // Truncating after 4 bytes would split 好 (3-byte sequence E5 A5 BD), so
  // helper should stop after 你 (3 bytes) at the last complete char boundary.
  char buf[5] = {};
  size_t total = WriteUtf8AsCharset(kNiHaoUtf8, "UTF-8", buf, sizeof(buf));
  EXPECT_EQ(total, 6u);  // total length unaffected by buffer
  // bytes written: 3 ("你"), then NUL
  EXPECT_EQ(buf[3], '\0');
  EXPECT_EQ(0, std::memcmp(buf, kNiHaoUtf8.data(), 3));
}

TEST(EncodingTest, Utf8NoTruncationWhenFits) {
  char buf[16] = {};
  size_t total = WriteUtf8AsCharset(kNiHaoUtf8, "UTF-8", buf, sizeof(buf));
  EXPECT_EQ(total, kNiHaoUtf8.size());
  EXPECT_EQ('\0', buf[kNiHaoUtf8.size()]);
}

TEST(EncodingTest, GbkConversionRoundTrip) {
  // Skip on platforms where GBK is unavailable: helper falls back to UTF-8 and
  // returns the UTF-8 byte length, so we can detect that case.
  char buf[16] = {};
  size_t total = WriteUtf8AsCharset(kNiHaoUtf8, "GBK", buf, sizeof(buf));
  if (total == kNiHaoUtf8.size()) {
    GTEST_SKIP() << "GBK not available on this platform; helper fell back to "
                    "UTF-8.";
  }
  EXPECT_EQ(total, kNiHaoGbk.size());
  EXPECT_EQ(0, std::memcmp(buf, kNiHaoGbk.data(), kNiHaoGbk.size()));
  EXPECT_EQ('\0', buf[kNiHaoGbk.size()]);
}

TEST(EncodingTest, GbkCharsetCaseInsensitive) {
  char a[16] = {};
  char b[16] = {};
  size_t ta = WriteUtf8AsCharset(kNiHaoUtf8, "GBK", a, sizeof(a));
  size_t tb = WriteUtf8AsCharset(kNiHaoUtf8, "gbk", b, sizeof(b));
  EXPECT_EQ(ta, tb);
  if (ta == kNiHaoGbk.size()) {
    EXPECT_EQ(0, std::memcmp(a, b, ta));
  }
}

TEST(EncodingTest, GbkBoundaryTruncation) {
  // "你好" → 4 bytes in GBK. Buffer holds 3 bytes (room for 1 char + NUL).
  // The helper must stop after 1 char (2 bytes) and not split a multi-byte
  // sequence.
  char buf[3] = {};
  size_t total = WriteUtf8AsCharset(kNiHaoUtf8, "GBK", buf, sizeof(buf));
  if (total == kNiHaoUtf8.size()) {
    GTEST_SKIP() << "GBK not available on this platform.";
  }
  EXPECT_EQ(total, kNiHaoGbk.size());  // total reported, not capped
  // Buffer should contain "你" (2 bytes) + NUL — never the partial 3 bytes.
  EXPECT_EQ('\0', buf[2]);
  EXPECT_EQ(0, std::memcmp(buf, kNiHaoGbk.data(), 2));
}

TEST(EncodingTest, EmptyInputProducesEmptyOutput) {
  char buf[8];
  std::memset(buf, 'X', sizeof(buf));
  size_t total = WriteUtf8AsCharset("", "GBK", buf, sizeof(buf));
  EXPECT_EQ(total, 0u);
  EXPECT_EQ('\0', buf[0]);
}

TEST(EncodingTest, UnsupportedCharsetFallsBackToUtf8) {
  char buf[16] = {};
  size_t total = WriteUtf8AsCharset(kNiHaoUtf8, "DEFINITELY-NOT-A-REAL-CHARSET",
                                    buf, sizeof(buf));
  // Fallback writes UTF-8 bytes verbatim.
  EXPECT_EQ(total, kNiHaoUtf8.size());
  EXPECT_EQ(0, std::memcmp(buf, kNiHaoUtf8.data(), kNiHaoUtf8.size()));
}

TEST(EncodingTest, ZeroSizedBufferReportsLengthOnly) {
  // Passing buffer_size == 0 must not write anything but should still return
  // the converted length so callers can size a buffer.
  size_t total = WriteUtf8AsCharset(kNiHaoUtf8, "UTF-8", nullptr, 0);
  EXPECT_EQ(total, kNiHaoUtf8.size());
}

// ---------------------------------------------------------------------------
// Helpers used by SQLGetData's segmented read: whole-value encoding plus the
// character model the segmenter is allowed to rely on.
// ---------------------------------------------------------------------------

using maxcompute_odbc::encoding::CharPayload;
using maxcompute_odbc::encoding::EncodeCharPayload;
using maxcompute_odbc::encoding::Utf16FromUtf8;
using maxcompute_odbc::fetch::Dbcs;

TEST(EncodeCharPayloadTest, Utf8IsPassedThroughAndReportedAsUtf8) {
  CharPayload p = EncodeCharPayload(kNiHaoUtf8, "UTF-8");
  EXPECT_TRUE(p.utf8);
  EXPECT_EQ(p.dbcs, Dbcs::None);
  EXPECT_EQ(p.bytes, kNiHaoUtf8);
}

TEST(EncodeCharPayloadTest, GbkReportsItsTwoByteModel) {
  CharPayload p = EncodeCharPayload(kNiHaoUtf8, "gbk");
  if (p.utf8) {
    GTEST_SKIP() << "GBK not available on this platform; helper fell back to "
                    "UTF-8.";
  }
  EXPECT_EQ(p.dbcs, Dbcs::Gbk);
  EXPECT_EQ(p.bytes, kNiHaoGbk);
  // Whole value, nothing truncated: the caller decides where to cut.
  EXPECT_EQ(p.bytes.size(), kNiHaoGbk.size());
}

TEST(EncodeCharPayloadTest, UnknownCharsetFallsBackToUtf8) {
  CharPayload p =
      EncodeCharPayload(kNiHaoUtf8, "DEFINITELY-NOT-A-REAL-CHARSET");
  EXPECT_TRUE(p.utf8);
  EXPECT_EQ(p.dbcs, Dbcs::None);
  EXPECT_EQ(p.bytes, kNiHaoUtf8);
}

TEST(EncodeCharPayloadTest, AgreesWithWriteUtf8AsCharsetOnWhatFits) {
  // The bound-column path (WriteUtf8AsCharset) and the segmented path
  // (EncodeCharPayload) must report the same total length for the same value,
  // otherwise a column reads differently depending on which function the
  // application called.
  const char *charsets[] = {"UTF-8", "GBK", "BIG5", "SHIFT_JIS", "EUC-KR"};
  for (const char *charset : charsets) {
    char buf[64] = {};
    size_t total = WriteUtf8AsCharset(kNiHaoUtf8, charset, buf, sizeof(buf));
    CharPayload p = EncodeCharPayload(kNiHaoUtf8, charset);
    EXPECT_EQ(total, p.bytes.size()) << "charset=" << charset;
    EXPECT_EQ(0, std::memcmp(buf, p.bytes.data(), total))
        << "charset=" << charset;
  }
}

TEST(Utf16FromUtf8Test, AsciiAndBmp) {
  EXPECT_EQ(Utf16FromUtf8("abc"), (std::vector<uint16_t>{'a', 'b', 'c'}));
  EXPECT_EQ(Utf16FromUtf8(kNiHaoUtf8), (std::vector<uint16_t>{0x4F60, 0x597D}));
}

TEST(Utf16FromUtf8Test, AstralCodepointBecomesSurrogatePair) {
  // U+1F600 is F0 9F 98 80 in UTF-8.
  EXPECT_EQ(Utf16FromUtf8("\xF0\x9F\x98\x80"),
            (std::vector<uint16_t>{0xD83D, 0xDE00}));
}

TEST(Utf16FromUtf8Test, MalformedBytesBecomeOneReplacementEach) {
  // A lone continuation byte, then the two leading bytes of a 3-byte sequence
  // that never completes: one U+FFFD per byte that could not be decoded, so a
  // caller can always tell that something was dropped.
  const std::vector<uint16_t> units = Utf16FromUtf8("\x80\xE4\xBD");
  ASSERT_EQ(units.size(), 3u);
  EXPECT_EQ(units[0], 0xFFFD);
  EXPECT_EQ(units[1], 0xFFFD);
  EXPECT_EQ(units[2], 0xFFFD);
}

TEST(Utf16FromUtf8Test, OverlongEncodingIsRejectedNotDecoded) {
  // C0 80 is the overlong form of NUL.
  const std::vector<uint16_t> units = Utf16FromUtf8("\xC0\x80");
  ASSERT_EQ(units.size(), 2u);
  EXPECT_EQ(units[0], 0xFFFD);
  EXPECT_EQ(units[1], 0xFFFD);
}

TEST(Utf16FromUtf8Test, EmptyInputProducesNoUnits) {
  EXPECT_TRUE(Utf16FromUtf8("").empty());
}
