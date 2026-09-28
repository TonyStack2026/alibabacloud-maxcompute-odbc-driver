/*
 * fetch_contract.c - a minimal ODBC consumer for SQLGetData's long-data
 * contract, as seen through a real Driver Manager (unixODBC on Linux/macOS).
 *
 *   cc -Wall -o fetch_contract test/e2e/fetch_contract.c -lodbc
 *
 * Connection settings are taken from the environment, the same variables the
 * Python e2e suite uses:
 *
 *   MCO_CONNSTR              full ODBC connection string (wins if set)
 *   MCO_DRIVER_PATH          path to libmaxcompute_odbc.so / .dylib
 *   MAXCOMPUTE_ENDPOINT      e.g. https://service.cn-shanghai.maxcompute.aliyun.com/api
 *   MAXCOMPUTE_PROJECT
 *   ALIBABA_CLOUD_ACCESS_KEY_ID / ALIBABA_CLOUD_ACCESS_KEY_SECRET
 *
 * Every check prints one line; the exit code is the number of failed checks.
 * The queries use SELECT ... without FROM, so no table is created and nothing
 * needs cleaning up.
 *
 * Two things this probe learned the hard way, and encodes on purpose:
 *   - a null TargetValuePtr is rejected by the Driver Manager itself (HY009),
 *     so the "how long is it" call has to pass a real buffer with
 *     BufferLength 0;
 *   - unixODBC refuses SQLExecDirect while a cursor is open, so reusing a
 *     statement goes through SQLFreeStmt(SQL_CLOSE) first, which is also the
 *     path pyodbc and the CLI tools take.
 */

#include <sql.h>
#include <sqlext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                        \
  do {                                          \
    if (cond) {                                 \
      fputs("  ok   ", stdout);                 \
    } else {                                    \
      fputs("  FAIL", stdout);                  \
      ++g_failures;                             \
    }                                           \
    printf(__VA_ARGS__);                        \
    printf("\n");                               \
    fflush(stdout);                             \
  } while (0)

static void print_diag(SQLSMALLINT type, SQLHANDLE handle, const char *when) {
  SQLCHAR state[8] = {0}, msg[1024] = {0};
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  if (SQLGetDiagRec(type, handle, 1, state, &native, msg,
                    (SQLSMALLINT)sizeof(msg), &len) == SQL_SUCCESS) {
    printf("       diag @ %s: state=%s msg=%s\n", when, state, msg);
  }
}

static char *conn_string(void) {
  const char *whole = getenv("MCO_CONNSTR");
  if (whole && *whole) return strdup(whole);

  const char *driver = getenv("MCO_DRIVER_PATH");
  const char *endpoint = getenv("MAXCOMPUTE_ENDPOINT");
  const char *project = getenv("MAXCOMPUTE_PROJECT");
  const char *id = getenv("ALIBABA_CLOUD_ACCESS_KEY_ID");
  const char *secret = getenv("ALIBABA_CLOUD_ACCESS_KEY_SECRET");
  if (!driver || !*driver || !project || !*project || !id || !*id || !secret ||
      !*secret) {
    fprintf(stderr,
            "set MCO_CONNSTR, or MCO_DRIVER_PATH + MAXCOMPUTE_ENDPOINT/"
            "MAXCOMPUTE_PROJECT + ALIBABA_CLOUD_ACCESS_KEY_ID/_SECRET\n");
    exit(2);
  }
  char *out = malloc(2048);
  snprintf(out, 2048,
           "DRIVER={%s};Endpoint=%s;Project=%s;AccessKeyId=%s;"
           "AccessKeySecret=%s;interactiveMode=false;",
           driver, endpoint && *endpoint ? endpoint : "", project, id, secret);
  return out;
}

/* Fill `want` with `c` repeated n times (C string). */
static void fill(char *want, size_t cap, char c, size_t n) {
  if (n >= cap) n = cap - 1;
  memset(want, c, n);
  want[n] = '\0';
}

/* Read one column in parts with a fixed BufferLength, concatenating what came
 * back. Returns the number of calls and records the sequence of return codes
 * and indicators for the caller to assert on. */
#define MAX_CALLS 512

struct ReadLog {
  SQLRETURN codes[MAX_CALLS];
  SQLLEN indicators[MAX_CALLS];
  int calls;
  int saw_01004;
  int progress_stalled;
  char out[16384];
  size_t out_len;
};

static SQLRETURN read_parts(SQLHSTMT stmt, SQLUSMALLINT col, SQLSMALLINT ctype,
                            SQLLEN step, struct ReadLog *log) {
  SQLRETURN ret = SQL_SUCCESS;
  char buf[512];
  if (step > (SQLLEN)sizeof(buf)) step = (SQLLEN)sizeof(buf);
  log->calls = 0;
  log->saw_01004 = 0;
  log->progress_stalled = 0;
  log->out_len = 0;
  log->out[0] = '\0';

  do {
    SQLLEN ind = -1;
    memset(buf, 0, sizeof(buf));
    ret = SQLGetData(stmt, col, ctype, buf, step, &ind);
    if (log->calls < MAX_CALLS) {
      log->codes[log->calls] = ret;
      log->indicators[log->calls] = ind;
    }
    ++log->calls;
    if (ret == SQL_SUCCESS_WITH_INFO) {
      SQLCHAR state[8] = {0};
      SQLINTEGER native = 0;
      SQLSMALLINT len = 0;
      SQLCHAR msg[256] = {0};
      if (SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &native, msg,
                        (SQLSMALLINT)sizeof(msg), &len) == SQL_SUCCESS &&
          strcmp((char *)state, "01004") == 0) {
        log->saw_01004 = 1;
      }
      SQLLEN copied = (ind >= 0 && step > 1) ? step - 1 : 0;
      /* The last segment is short; find it by string length for text and by
       * the reported indicator for binary. Good enough for the checks below,
       * which only care that data moved. */
      if (ctype == SQL_C_CHAR) copied = (SQLLEN)strlen(buf);
      if (copied <= 0) log->progress_stalled = 1;
      size_t add = (size_t)(copied > 0 ? copied : 0);
      if (log->out_len + add < sizeof(log->out)) {
        memcpy(log->out + log->out_len, buf, add);
        log->out_len += add;
        log->out[log->out_len] = '\0';
      }
    } else if (ret == SQL_SUCCESS) {
      SQLLEN add = (ctype == SQL_C_CHAR) ? (SQLLEN)strlen(buf)
                                         : (ind >= 0 ? ind : 0);
      if (add < 0) add = 0;
      if (log->out_len + (size_t)add < sizeof(log->out)) {
        memcpy(log->out + log->out_len, buf, (size_t)add);
        log->out_len += (size_t)add;
        log->out[log->out_len] = '\0';
      }
    }
    if (log->calls >= MAX_CALLS) break;
  } while (ret == SQL_SUCCESS_WITH_INFO);

  return ret;
}

static int query(SQLHSTMT stmt, const char *sql, const char *what) {
  SQLRETURN ret = SQLExecDirect(stmt, (SQLCHAR *)sql, SQL_NTS);
  if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
    printf("  SQLExecDirect failed for %s\n", what);
    print_diag(SQL_HANDLE_STMT, stmt, sql);
    return 0;
  }
  return 1;
}

int main(void) {
  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  char *cs = conn_string();
  SQLRETURN ret;

  if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env) != SQL_SUCCESS ||
      SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0) !=
          SQL_SUCCESS ||
      SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc) != SQL_SUCCESS) {
    fprintf(stderr, "cannot allocate handles\n");
    return 2;
  }
  ret = SQLDriverConnect(dbc, NULL, (SQLCHAR *)cs, SQL_NTS, NULL, 0, NULL,
                         SQL_DRIVER_NOPROMPT);
  if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
    printf("connect failed\n");
    print_diag(SQL_HANDLE_DBC, dbc, "SQLDriverConnect");
    return 2;
  }
  printf("connected\n");
  if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt) != SQL_SUCCESS) {
    fprintf(stderr, "cannot allocate statement\n");
    return 2;
  }

  /* ---------------------------------------------------------------- 0 */
  SQLUINTEGER ext = 0;
  SQLGetInfo(dbc, SQL_GETDATA_EXTENSIONS, &ext, 0, NULL);
  CHECK((ext & (SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER)) ==
            (SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER),
        "driver advertises the ordering it actually honours (0x%x)", ext);

  /* ---------------------------------------------------------------- 1 */
  char sql[4096];
  snprintf(sql, sizeof(sql),
           "SELECT REPEAT('a', 300) AS ascii_long,"
           " REPEAT('你好😀', 20) AS utf8_long,"
           " CAST(NULL AS STRING) AS null_str,"
           " '' AS empty_str,"
           " CAST(1234567890123 AS BIGINT) AS bigint_col;");
  if (!query(stmt, sql, "probe query")) return 2;
  ret = SQLFetch(stmt);
  if (ret != SQL_SUCCESS) {
    printf("probe query returned no row (ret=%d)\n", ret);
    print_diag(SQL_HANDLE_STMT, stmt, "SQLFetch");
    return 2;
  }

  struct ReadLog r;
  char want[1024];

  /* ------------------------------------------------- fixed-length column */
  SQLBIGINT num = 0;
  SQLLEN ind = -1;
  ret = SQLGetData(stmt, 5, SQL_C_SBIGINT, &num, 0, &ind);
  CHECK(ret == SQL_SUCCESS && num == 1234567890123LL &&
            ind == (SQLLEN)sizeof(SQLBIGINT),
        "BIGINT via SQL_C_SBIGINT: ret=%d value=%lld indicator=%lld", ret,
        (long long)num, (long long)ind);
  ret = SQLGetData(stmt, 5, SQL_C_SBIGINT, &num, 0, &ind);
  CHECK(ret == SQL_NO_DATA,
        "fixed-length data is not returned in parts: second call ret=%d", ret);

  /* --------------------------------------------------- NULL and empty */
  char scratch[16];
  ind = -1;
  ret = SQLGetData(stmt, 3, SQL_C_CHAR, scratch, sizeof(scratch), &ind);
  CHECK(ret == SQL_SUCCESS && ind == SQL_NULL_DATA,
        "NULL string: ret=%d indicator=%lld (SQL_NULL_DATA=%lld)", ret,
        (long long)ind, (long long)SQL_NULL_DATA);
  ret = SQLGetData(stmt, 3, SQL_C_CHAR, scratch, sizeof(scratch), NULL);
  CHECK(ret == SQL_ERROR,
        "NULL with no indicator variable is an error: ret=%d", ret);
  SQLCHAR state[8] = {0};
  SQLCHAR msg[256] = {0};
  SQLINTEGER native = 0;
  SQLSMALLINT mlen = 0;
  SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &native, msg,
                (SQLSMALLINT)sizeof(msg), &mlen);
  CHECK(strcmp((char *)state, "22002") == 0, "and reports 22002, got %s",
        state);

  ind = -1;
  memset(scratch, 'x', sizeof(scratch));
  ret = SQLGetData(stmt, 4, SQL_C_CHAR, scratch, sizeof(scratch), &ind);
  CHECK(ret == SQL_SUCCESS && ind == 0 && scratch[0] == '\0',
        "empty string: ret=%d indicator=%lld buffer=\"%s\"", ret,
        (long long)ind, scratch);
  ret = SQLGetData(stmt, 4, SQL_C_CHAR, scratch, sizeof(scratch), &ind);
  CHECK(ret == SQL_NO_DATA && ind == 0,
        "empty string then SQL_NO_DATA: ret=%d indicator=%lld", ret,
        (long long)ind);

  /* --------------------------------------------------- length discovery */
  /* A null TargetValuePtr is rejected by the Driver Manager (HY009), so length
   * discovery is a real buffer with BufferLength 0. */
  ret = SQLGetData(stmt, 1, SQL_C_CHAR, scratch, 0, &ind);
  CHECK(ret == SQL_SUCCESS_WITH_INFO && ind == 300,
        "BufferLength 0 reports the length: ret=%d indicator=%lld", ret,
        (long long)ind);

  /* --------------------------------------------------- ASCII in parts */
  ret = read_parts(stmt, 1, SQL_C_CHAR, 16, &r);
  fill(want, sizeof(want), 'a', 300);
  CHECK(r.calls >= 20 && r.codes[r.calls - 1] == SQL_SUCCESS,
        "300 bytes with a 16-byte buffer: %d calls, last ret=%d", r.calls,
        r.codes[r.calls - 1]);
  CHECK(r.out_len == 300 && strcmp(r.out, want) == 0,
        "reassembly is byte-exact (%zu bytes)", r.out_len);
  CHECK(r.saw_01004, "SQLGetDiagRec reported 01004 during the parts");
  int decreasing = 1;
  for (int i = 1; i < r.calls; ++i) {
    if (r.indicators[i] >= r.indicators[i - 1]) decreasing = 0;
  }
  CHECK(decreasing, "StrLen_or_Ind decreases call over call (%lld -> %lld)",
        (long long)r.indicators[0], (long long)r.indicators[r.calls - 1]);
  ret = SQLGetData(stmt, 1, SQL_C_CHAR, scratch, sizeof(scratch), &ind);
  CHECK(ret == SQL_NO_DATA,
        "after the last part, another call returns SQL_NO_DATA: ret=%d", ret);

  /* --------------------------------------------------- multi-byte in parts */
  ret = read_parts(stmt, 2, SQL_C_CHAR, 4, &r);
  CHECK(r.out_len == 200 && !r.progress_stalled,
        "UTF-8 value (3-byte + 4-byte sequences) read with a 4-byte buffer:"
        " %zu bytes, stalled=%d",
        r.out_len, r.progress_stalled);
  {
    /* 10 bytes: 3 + 3 + 4 */
    const char *unit = "\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x98\x80";
    size_t ul = strlen(unit);
    int same = r.out_len == ul * 20;
    for (size_t i = 0; i + ul <= r.out_len && same; i += ul) {
      if (memcmp(r.out + i, unit, ul) != 0) same = 0;
    }
    CHECK(same, "repeated multi-byte value reassembles exactly");
  }

  /* --------------------------------------------------- out-of-order read */
  ind = -1;
  ret = SQLGetData(stmt, 1, SQL_C_CHAR, scratch, 8, &ind);
  CHECK(ret == SQL_SUCCESS_WITH_INFO && ind == 300,
        "partial read of column 1 starts (ret=%d indicator=%lld)", ret,
        (long long)ind);
  char buf2[8];
  ind = -1;
  ret = SQLGetData(stmt, 4, SQL_C_CHAR, buf2, sizeof(buf2), &ind);
  CHECK(ret == SQL_SUCCESS && ind == 0,
        "column 4 read in between (ret=%d indicator=%lld)", ret,
        (long long)ind);
  ind = -1;
  ret = SQLGetData(stmt, 1, SQL_C_CHAR, scratch, 8, &ind);
  CHECK(ret == SQL_SUCCESS_WITH_INFO && ind == 300,
        "coming back to column 1 restarts it: ret=%d indicator=%lld", ret,
        (long long)ind);

  /* --------------------------------------------------- wide characters */
  {
    /* Column 1 from the top again (the previous call was for another column).
     * An 8-byte buffer holds 3 UTF-16 units plus the terminator, so the 300
     * 'a' characters need many calls. Counting the units that arrived is the
     * same arithmetic an application does when it reassembles parts. */
    const SQLLEN bytes = 8;
    const SQLLEN units_per_call = bytes / (SQLLEN)sizeof(SQLWCHAR) - 1;
    SQLWCHAR wbuf[8];
    /* The check above left an offset pending on column 1. Touch another column
     * first, because that is what invalidates the offset ("Successive calls to
     * SQLGetData will retrieve data from the last column requested; prior
     * offsets become invalid") and this block assumes a read from the top. */
    SQLGetData(stmt, 2, SQL_C_CHAR, scratch, sizeof(scratch), &ind);
    SQLLEN total_units = 0;
    int wcalls = 0;
    int wdone = 0;
    ret = SQL_SUCCESS_WITH_INFO;
    while (ret == SQL_SUCCESS_WITH_INFO && wcalls < 400) {
      SQLLEN wlen = -1;
      ret = SQLGetData(stmt, 1, SQL_C_WCHAR, wbuf, bytes, &wlen);
      ++wcalls;
      if (ret == SQL_SUCCESS_WITH_INFO) {
        total_units += units_per_call;
      } else if (ret == SQL_SUCCESS) {
        total_units += wlen / (SQLLEN)sizeof(SQLWCHAR);
        wdone = 1;
      } else {
        CHECK(0, "wide read ended with ret=%d", ret);
        wdone = 1;
      }
    }
    CHECK(wdone && total_units == 300,
          "SQL_C_WCHAR reads the same 300 characters in %d calls (%lld units)",
          wcalls, (long long)total_units);
    ret = SQLGetData(stmt, 1, SQL_C_WCHAR, wbuf, bytes, &ind);
    CHECK(ret == SQL_NO_DATA,
          "wide read then SQL_NO_DATA: ret=%d indicator=%lld", ret,
          (long long)ind);
  }

  /* --------------------------------------------------- bad column number */
  ret = SQLGetData(stmt, 0, SQL_C_CHAR, scratch, sizeof(scratch), &ind);
  CHECK(ret == SQL_ERROR, "column 0 is rejected: ret=%d", ret);

  /* --------------------------------------------------- statement reuse */
  /* unixODBC refuses SQLExecDirect on a handle whose cursor is still open, so
   * an application that reuses a statement closes the cursor first - exactly
   * what isql, pyodbc and BI tools do. The defect under test is what the driver
   * remembers afterwards: the old code kept the previous query's last row, its
   * end-of-stream flag and its SQLGetData offset, and SQLFreeStmt(SQL_CLOSE)
   * discarded none of them, so the second result set read as empty. */
  SQLFreeStmt(stmt, SQL_CLOSE);
  if (!query(stmt, "SELECT REPEAT('b', 40) AS second_col;", "second query"))
    return 2;
  ret = SQLFetch(stmt);
  CHECK(ret == SQL_SUCCESS,
        "the same statement handle can run a second query: SQLFetch ret=%d",
        ret);
  if (ret == SQL_SUCCESS) {
    ret = read_parts(stmt, 1, SQL_C_CHAR, 16, &r);
    fill(want, sizeof(want), 'b', 40);
    CHECK(r.out_len == 40 && strcmp(r.out, want) == 0,
          "second query's value is readable in full (%zu bytes)", r.out_len);
  }

  /* ------------------------------------- drained result, then reuse again */
  /* The variant that bites BI tools: the first query is read to the end before
   * the handle is reused. The old code kept the end-of-stream flag, so the
   * second result set had no rows at all. */
  while (SQLFetch(stmt) == SQL_SUCCESS) {
  }
  SQLFreeStmt(stmt, SQL_CLOSE);
  if (!query(stmt, "SELECT REPEAT('d', 33) AS fourth_col;", "fourth query"))
    return 2;
  ret = SQLFetch(stmt);
  CHECK(ret == SQL_SUCCESS,
        "a drained result must not make the next query empty: ret=%d", ret);
  if (ret == SQL_SUCCESS) {
    ret = read_parts(stmt, 1, SQL_C_CHAR, 16, &r);
    fill(want, sizeof(want), 'd', 33);
    CHECK(r.out_len == 33 && strcmp(r.out, want) == 0,
          "fourth query reads back in full (%zu bytes)", r.out_len);
  }

  /* --------------------------------------------------- bindings on reuse */
  /* A column bound against the *previous* schema must not be written into the
   * next result set's fetch, and unbinding has to take effect. */
  char bound[64];
  SQLLEN bound_ind = 0;
  memset(bound, 0, sizeof(bound));
  SQLBindCol(stmt, 1, SQL_C_CHAR, bound, sizeof(bound), &bound_ind);
  SQLFreeStmt(stmt, SQL_UNBIND);
  SQLFreeStmt(stmt, SQL_CLOSE);
  if (!query(stmt, "SELECT REPEAT('c', 25) AS third_col;", "third query"))
    return 2;
  ret = SQLFetch(stmt);
  CHECK(ret == SQL_SUCCESS, "third query after SQL_UNBIND: SQLFetch ret=%d",
        ret);
  CHECK(bound[0] == '\0',
        "an unbound column is not written on the next fetch (got \"%s\")",
        bound);
  ret = read_parts(stmt, 1, SQL_C_CHAR, 16, &r);
  fill(want, sizeof(want), 'c', 25);
  CHECK(r.out_len == 25 && strcmp(r.out, want) == 0,
        "third query reads back in full through SQLGetData (%zu bytes)",
        r.out_len);

  SQLFreeStmt(stmt, SQL_DROP);
  SQLDisconnect(dbc);
  SQLFreeHandle(SQL_HANDLE_DBC, dbc);
  SQLFreeHandle(SQL_HANDLE_ENV, env);

  printf("\n%s (%d failed checks)\n",
         g_failures ? "CONTRACT FAILURES" : "ALL CHECKS PASSED", g_failures);
  return g_failures > 0 ? 1 : 0;
}
