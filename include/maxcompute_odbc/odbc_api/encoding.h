#pragma once

#include "maxcompute_odbc/odbc_api/column_fetch.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace maxcompute_odbc::encoding {

/**
 * Convert a UTF-8 string into a target charset and write it to a SQL_C_CHAR
 * buffer.
 *
 * Behavior:
 * - Truncates at character boundaries (never writes a partial multi-byte
 *   character; the next byte after the last complete character is the NUL).
 * - Always NUL-terminates the buffer when buffer_size > 0.
 * - Returns the *total* converted byte length (excluding NUL), so callers
 *   can fill SQL_LEN_or_Ind correctly. If the return value is greater than
 *   buffer_size - 1, the data was truncated.
 *
 * Recognized charset values (case-insensitive, normalized to UPPER):
 *   "UTF-8" / "UTF8"  -> no conversion (UTF-8 byte truncation only)
 *   "GBK" / "CP936" / "GB2312" -> Windows code page 936
 *   "GB18030"
 *   "BIG5" / "CP950"
 *   "SHIFT_JIS" / "SJIS" / "CP932"
 *   "EUC-KR" / "CP949"
 * Unknown values fall through to iconv (Unix) / numeric code-page parsing
 * (Windows). On unsupported charset or conversion error, the function falls
 * back to writing UTF-8 bytes (with character-boundary truncation) and logs a
 * warning at most once per process per unsupported charset.
 *
 * @param utf8        Source UTF-8 string.
 * @param charset     Target charset name.
 * @param buffer      Destination buffer (may be nullptr if buffer_size == 0).
 * @param buffer_size Size of the destination buffer in bytes.
 * @return Total byte length the converted output requires (excluding NUL).
 */
size_t WriteUtf8AsCharset(const std::string &utf8, const std::string &charset,
                          char *buffer, size_t buffer_size);

/**
 * Convert a UTF-8 string into a target charset without truncating anything.
 *
 * The non-truncating sibling of WriteUtf8AsCharset: it returns the whole
 * converted byte string so that callers can slice it themselves. Charset
 * recognition, case handling and the "unknown charset falls back to UTF-8"
 * rule are the same as in WriteUtf8AsCharset.
 */
std::string ConvertUtf8ToCharset(const std::string &utf8,
                                 const std::string &charset);

/**
 * Decode a UTF-8 string into native-endian UTF-16 code units (SQLWCHAR).
 *
 * - Code points above the BMP become surrogate pairs.
 * - A malformed or truncated byte sequence becomes one U+FFFD per skipped
 *   byte, so input bytes never disappear without leaving a character.
 * - No length limit is imposed; truncation is the caller's job.
 */
std::vector<std::uint16_t> Utf16FromUtf8(const std::string &utf8);

/**
 * Encode a value for a SQL_C_CHAR buffer and report how to cut it safely.
 *
 * `bytes` holds the converted value with no terminator. `utf8` says the output
 * really is UTF-8 (the empty charset, UTF-8/UTF8, or a charset that fell back
 * to UTF-8 because the platform could not convert it), which lets the caller
 * keep segments on UTF-8 codepoint boundaries. `dbcs` names the two-byte
 * model when the output uses one, and is Dbcs::None for single-byte output and
 * for multi-byte models the segmenter cannot walk (GB18030, and charsets only
 * iconv or a Windows code page recognises): there a cut may land inside a
 * character and only byte-exact reassembly is promised.
 */
struct CharPayload {
  std::string bytes;
  bool utf8 = true;
  fetch::Dbcs dbcs = fetch::Dbcs::None;
};
CharPayload EncodeCharPayload(const std::string &utf8,
                              const std::string &charset);

}  // namespace maxcompute_odbc::encoding
