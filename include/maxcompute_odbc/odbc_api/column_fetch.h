#pragma once
// Pure logic behind SQLGetData when a value does not fit in the application
// buffer. Deliberately free of statement handles, iconv and the MaxCompute
// client so that the whole contract can be unit tested without building the
// driver or opening a connection.
//
// The contract implemented here is the one in the ODBC SQLGetData reference,
// "Retrieving Variable-Length Data in Parts" plus the numbered steps under
// "Retrieving Data with SQLGetData":
//   * a repeated call for the same column returns the *next* part;
//   * *StrLen_or_IndPtr is the length available at the start of the current
//     call, so it decreases with every subsequent call;
//   * a call that could not return everything answers SQL_SUCCESS_WITH_INFO
//     with SQLSTATE 01004; the call that returns the last part answers
//     SQL_SUCCESS - never SQL_NO_TOTAL and never zero - so that the
//     application knows how many bytes of its buffer are valid;
//   * a call after everything has been returned answers SQL_NO_DATA;
//   * fixed-length data cannot be returned in parts: every call after the
//     first answers SQL_NO_DATA;
//   * "Successive calls to SQLGetData will retrieve data from the last column
//     requested; prior offsets become invalid."

#include "maxcompute_odbc/platform.h"  // Must include before sql.h on Windows
#include <cstddef>
#include <cstdint>
#include <sql.h>
#include <sqlext.h>
#include <vector>

namespace maxcompute_odbc {
namespace fetch {

// Which column SQLGetData was last called for on this statement and how much
// of it has already been handed over. One cursor per statement, not per
// column: switching columns invalidates the previous offset.
struct Cursor {
  SQLUSMALLINT column = 0;  // 0 == nothing has been read yet
  std::size_t offset = 0;   // payload bytes already returned for `column`
  bool exhausted = false;   // the last part of `column` has been returned

  void reset() {
    column = 0;
    offset = 0;
    exhausted = false;
  }
};

// Multi-byte character models this module knows how to keep intact across a
// segment boundary. Selected from the client charset by
// encoding::GetDbcsShape(); the byte tables live in column_fetch.cpp so that
// segmentation stays testable without iconv.
enum class Dbcs : std::uint8_t {
  None = 0,  // 1 byte per character, or a model we do not know (GB18030,
             // Windows ANSI code pages): segments may cut anywhere. Reassembly
             // is still byte-exact.
  Gbk,       // lead 0x81-0xFE + one trail byte (GBK, GB2312, EUC-KR/CP949)
  Big5,      // lead 0xA1-0xF9 + one trail byte
  Sjis,      // lead 0x81-0x9F or 0xE0-0xEF + one trail byte
};

// Shape of the payload, derived by the caller from the C target type and the
// client charset.
struct Layout {
  // Atomic unit of the payload: 1 for byte-ish data (SQL_C_CHAR,
  // SQL_C_BINARY, fixed-length types), 2 for UTF-16 code units (SQL_C_WCHAR).
  std::uint8_t element_bytes = 1;
  // Character payloads reserve space for one terminator: one byte for
  // SQL_C_CHAR, one UTF-16 code unit for SQL_C_WCHAR. Binary and fixed-length
  // payloads carry no terminator.
  bool nul_terminate = true;
  // Payload is UTF-8: never cut in front of a continuation byte.
  bool utf8_boundary = false;
  // Payload is UTF-16: cut on a code-unit boundary and never between the two
  // halves of a surrogate pair.
  bool utf16_boundary = false;
  // Payload is a 2-byte DBCS charset.
  Dbcs dbcs = Dbcs::None;
};

struct Outcome {
  SQLRETURN ret = SQL_SUCCESS;
  // Value the caller stores in StrLen_or_IndPtr (only when indicator_valid).
  SQLLEN indicator = 0;
  bool indicator_valid = false;
  // True when the caller must add a SQLSTATE 01004 diagnostic record.
  bool truncated = false;
  // Payload bytes copied by this call, excluding the terminator.
  std::size_t copied = 0;
};

// Copy the next segment of `payload` into the application buffer and advance
// `cursor`. If the cursor belongs to another column it is restarted first.
//
// `buffer_length` is in bytes, like SQLGetData's BufferLength. 0 means "report
// the length, copy nothing" and consumes nothing, so the value can still be
// read from the start afterwards.
//
// A segment always ends on a character boundary when one is reachable; if the
// next whole character does not fit in the space left, the character is split
// instead of returning zero bytes, so that every call with a non-zero buffer
// makes progress and an application cannot spin.
Outcome CopySegment(const std::vector<std::uint8_t> &payload,
                    const Layout &layout, Cursor &cursor, SQLUSMALLINT column,
                    void *buffer, SQLLEN buffer_length);

}  // namespace fetch
}  // namespace maxcompute_odbc
