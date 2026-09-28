"""
长文本 / NULL / 空串的分段读取测试 (pyodbc 视角).

pyodbc 对变长字符列会用 SQLGetData 反复取值: 一次给不完时按返回码继续,
直到拿到完整值. 这组用例不检查 SQLRETURN 本身 (那是 fetch_contract.c 的活),
它检查的是"真实消费者最后拿到的内容是否完整", 并在 max_length 很小、必须
分段时重复一遍.

覆盖: 长 ASCII、长多字节 (含 4 字节 UTF-8 / 代理对)、NULL、空串、
同一个 cursor 上连续执行两条查询 (stmt 复用).
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pyodbc
from base_test import BaseTest, print_section
from config import config


# 300 x 'a'
ASCII_LONG = "a" * 300
# "你好" + emoji (U+1F600, 需要 UTF-16 代理对) 重复 20 次
MULTIBYTE_LONG = "\u4f60\u597d\U0001f600" * 20


class TestLongData(BaseTest):
    """SQLGetData 分段读取的端到端测试"""

    def _connect(self, max_length=None) -> pyodbc.Connection:
        conn_str = (
            f"DRIVER={{MaxCompute ODBC Driver}};"
            f"Endpoint={config.endpoint};"
            f"Project={config.project};"
            f"AccessKeyId={config.access_key_id};"
            f"AccessKeySecret={config.access_key_secret};"
            f"interactiveMode=false;"
        )
        conn = pyodbc.connect(conn_str)
        if max_length:
            # 小缓冲区 -> 强制应用把一个值分成很多次 SQLGetData 读取
            conn.max_length = max_length
        return conn

    def _select_literal(self, expr: str) -> str:
        return f"SELECT {expr} AS value;"

    def test_ascii_long(self):
        """长 ASCII 字符串完整返回"""
        conn = self._connect()
        try:
            row = conn.cursor().execute(
                self._select_literal(f"REPEAT('a', {len(ASCII_LONG)})")
            ).fetchone()
            self.assert_equals(row[0], ASCII_LONG, "long ASCII value")
        finally:
            conn.close()

    def test_ascii_long_forced_small_buffers(self):
        """max_length=64 时驱动仍要给出完整值 (分段读)"""
        conn = self._connect(max_length=64)
        try:
            row = conn.cursor().execute(
                self._select_literal(f"REPEAT('a', {len(ASCII_LONG)})")
            ).fetchone()
            self.assert_equals(
                row[0], ASCII_LONG,
                "long ASCII value with a 64-byte application buffer")
        finally:
            conn.close()

    def test_multibyte_long(self):
        """含 3 字节与 4 字节 UTF-8 序列的长值完整返回"""
        expected = MULTIBYTE_LONG
        conn = self._connect()
        try:
            row = conn.cursor().execute(
                self._select_literal("REPEAT('\u4f60\u597d\U0001f600', 20)")
            ).fetchone()
            self.assert_equals(row[0], expected, "multi-byte value")
        finally:
            conn.close()

    def test_multibyte_long_forced_small_buffers(self):
        """同样的多字节值, 但小缓冲区 -> 分段拼接后仍逐字符相等"""
        conn = self._connect(max_length=16)
        try:
            row = conn.cursor().execute(
                self._select_literal("REPEAT('\u4f60\u597d\U0001f600', 20)")
            ).fetchone()
            self.assert_equals(
                row[0], MULTIBYTE_LONG,
                "multi-byte value with a 16-byte application buffer")
            # 分段边界的处理不应留下替换字符或半个字符
            self.assert_equals(
                row[0].count("\ufffd"), 0,
                "no replacement characters introduced by the segmentation")
        finally:
            conn.close()

    def test_null_and_empty(self):
        """NULL 返回 None, 空串返回 ''"""
        conn = self._connect()
        try:
            cur = conn.cursor()
            row = cur.execute(
                "SELECT CAST(NULL AS STRING) AS null_col, "
                "'' AS empty_col, REPEAT('z', 500) AS long_col"
            ).fetchone()
            self.assert_equals(row[0], None, "NULL column")
            self.assert_equals(row[1], "", "empty string column")
            self.assert_equals(row[2], "z" * 500, "long column in same row")
        finally:
            conn.close()

    def test_statement_reuse(self):
        """同一个 cursor 上跑第二条查询, 必须拿到第二条的结果"""
        conn = self._connect()
        try:
            cur = conn.cursor()
            cur.execute("SELECT REPEAT('a', 300) AS first_col")
            first = cur.fetchone()
            self.assert_equals(first[0], ASCII_LONG, "first query value")
            # 读完第一行
            self.assert_equals(cur.fetchone(), None, "first result drained")

            cur.execute("SELECT REPEAT('b', 40) AS second_col")
            second = cur.fetchone()
            self.assert_not_none(
                second,
                "second query on the same cursor must return a row")
            self.assert_equals(second[0], "b" * 40, "second query value")
        finally:
            conn.close()

    def test_long_column_ordering(self):
        """一行里多个长列, 顺序读取互不干扰"""
        conn = self._connect(max_length=32)
        try:
            row = conn.cursor().execute(
                "SELECT REPEAT('m', 200) AS c1, REPEAT('n', 100) AS c2, "
                "REPEAT('o', 300) AS c3"
            ).fetchone()
            self.assert_equals(row[0], "m" * 200, "c1")
            self.assert_equals(row[1], "n" * 100, "c2")
            self.assert_equals(row[2], "o" * 300, "c3")
        finally:
            conn.close()


if __name__ == "__main__":
    test = TestLongData()
    print_section("Long data / segmented SQLGetData E2E Tests")
    test.run_test(test.test_ascii_long, "long ASCII value")
    test.run_test(test.test_ascii_long_forced_small_buffers,
                  "long ASCII with a 64-byte buffer")
    test.run_test(test.test_multibyte_long, "multi-byte value")
    test.run_test(test.test_multibyte_long_forced_small_buffers,
                  "multi-byte with a 16-byte buffer")
    test.run_test(test.test_null_and_empty, "NULL and empty string")
    test.run_test(test.test_statement_reuse, "statement reuse")
    test.run_test(test.test_long_column_ordering, "several long columns")
    success = test.print_results()
    sys.exit(0 if success else 1)
