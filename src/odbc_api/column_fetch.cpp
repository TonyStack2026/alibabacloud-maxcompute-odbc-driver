#include "maxcompute_odbc/odbc_api/column_fetch.h"
#include <algorithm>
#include <cstring>

namespace maxcompute_odbc {
namespace fetch {
namespace {

bool IsUtf8Continuation(std::uint8_t b) { return (b & 0xC0) == 0x80; }

// Two-byte character models, by lead byte. Trail bytes are only consumed
// blindly (they can overlap the ASCII range, so they must never be
// re-interpreted on their own - which is why the scan below always steps
// lead+trail together).
bool IsDbcsLead(Dbcs shape, std::uint8_t b) {
  switch (shape) {
    case Dbcs::Gbk:
      return b >= 0x81 && b <= 0xFE;
    case Dbcs::Big5:
      return b >= 0xA1 && b <= 0xF9;
    case Dbcs::Sjis:
      return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEF);
    case Dbcs::None:
      return false;
  }
  return false;
}

std::uint16_t UnitAt(const std::vector<std::uint8_t> &payload, std::size_t at) {
  // Read a native-endian SQLWCHAR: the payload was produced by writing
  // SQLWCHAR values, so its byte order is the platform's.
  SQLWCHAR unit = 0;
  std::memcpy(&unit, payload.data() + at, sizeof(unit));
  return static_cast<std::uint16_t>(unit);
}

bool IsHighSurrogate(std::uint16_t u) { return u >= 0xD800 && u <= 0xDBFF; }

// Largest byte count <= `want` that ends on a character boundary of `payload`
// (relative to `offset`). Returns `want` unchanged when the cut is at the end
// of the payload, because that is always a boundary.
std::size_t SnapToBoundary(const std::vector<std::uint8_t> &payload,
                           std::size_t offset, std::size_t want,
                           const Layout &layout) {
  const std::size_t total = payload.size();
  std::size_t cut = offset + want;
  if (cut >= total) return want;

  if (layout.element_bytes > 1) {
    while (cut > offset && ((cut - offset) % layout.element_bytes) != 0) {
      --cut;
    }
    if (layout.utf16_boundary && cut >= offset + layout.element_bytes) {
      // Do not end a segment right after the high half of a surrogate pair.
      std::size_t last = cut - layout.element_bytes;
      if (IsHighSurrogate(UnitAt(payload, last))) cut = last;
    }
    return cut - offset;
  }

  if (layout.utf8_boundary) {
    while (cut > offset && IsUtf8Continuation(payload[cut])) --cut;
    return cut - offset;
  }

  if (layout.dbcs != Dbcs::None) {
    std::size_t pos = offset;
    while (pos < cut) {
      std::size_t step = IsDbcsLead(layout.dbcs, payload[pos]) ? 2 : 1;
      if (pos + step > cut) break;  // this character does not fit before cut
      pos += step;
    }
    return pos - offset;
  }

  return want;
}

}  // namespace

Outcome CopySegment(const std::vector<std::uint8_t> &payload,
                    const Layout &layout, Cursor &cursor, SQLUSMALLINT column,
                    void *buffer, SQLLEN buffer_length) {
  Outcome out;

  // A different column invalidates the previous offset.
  if (cursor.column != column) {
    cursor.column = column;
    cursor.offset = 0;
    cursor.exhausted = false;
  }

  if (cursor.exhausted) {
    out.ret = SQL_NO_DATA;
    out.indicator = 0;
    out.indicator_valid = true;
    return out;
  }

  const std::size_t total = payload.size();
  if (cursor.offset > total) cursor.offset = total;  // defensive
  const std::size_t remaining = total - cursor.offset;

  out.indicator = static_cast<SQLLEN>(remaining);
  out.indicator_valid = true;

  const std::size_t term = layout.nul_terminate ? layout.element_bytes : 0;

  // BufferLength == 0 (or no buffer at all) is the documented way to ask for
  // the length without consuming anything. The value still has to fit a
  // terminator to be reportable as complete, so anything left over is a
  // truncation warning.
  if (buffer == nullptr || buffer_length <= 0) {
    if (remaining == 0) {
      cursor.exhausted = true;
      out.ret = SQL_SUCCESS;
    } else {
      out.ret = SQL_SUCCESS_WITH_INFO;
      out.truncated = true;
    }
    return out;
  }

  const std::size_t capacity = static_cast<std::size_t>(buffer_length);
  const std::size_t max_copy = capacity > term ? capacity - term : 0;
  std::size_t want = std::min(max_copy, remaining);
  std::size_t cut = SnapToBoundary(payload, cursor.offset, want, layout);

  if (cut == 0 && want >= layout.element_bytes) {
    // The next whole character does not fit into what is left of the buffer.
    // Hand over whole elements anyway: an application that reads a value in
    // parts reassembles them, and returning nothing at all here would let it
    // loop forever on the same offset.
    cut = want - (want % layout.element_bytes);
  }

  if (cut > 0) {
    std::memcpy(buffer, payload.data() + cursor.offset, cut);
    cursor.offset += cut;
    out.copied = cut;
  }

  if (layout.nul_terminate && term > 0 && cut + term <= capacity) {
    std::memset(static_cast<char *>(buffer) + cut, 0, term);
  }

  if (cursor.offset < total) {
    out.ret = SQL_SUCCESS_WITH_INFO;
    out.truncated = true;
  } else {
    out.ret = SQL_SUCCESS;
    cursor.exhausted = true;
  }
  return out;
}

}  // namespace fetch
}  // namespace maxcompute_odbc
