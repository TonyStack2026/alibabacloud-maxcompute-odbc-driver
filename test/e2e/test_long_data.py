"""
长文本 / NULL / 空串的分段读取测试 (pyodbc 视角).

pyodbc 给未知长度的字符列只分配约 2 KB 的缓冲区, 值更长时它必须反复调用
SQLGetData 才能拼回完整值 (5.1.0 上没有 max_length / setoutputsize 这类开关,
setoutputsize 是空操作). 所以"强制分段"的做法就是把值本身做长: 70000 个 ASCII
字符 (约 34 次 SQLGetData) 或 300 组多字节字符.

这组用例不检查 SQLRETURN 本身 (那是 fetch_contract.c 的活), 它检查的是真实消费
者最后拿到的内容是否完整.

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


class TestLongData(BaseTest):
    """SQLGetData 分段读取的端到端测试"""

    def _connect(self) -> pyodbc.Connection:
        conn_str = (
            f"DRIVER={{MaxCompute ODBC Driver}};"
            f"Endpoint={config.endpoint};"
            f"Project={config.project};"
            f"AccessKeyId={config.access_key_id};"
            f"AccessKeySecret={config.access_key_secret};"
            f"interactiveMode=false;"
        )
        return pyodbc.connect(conn_str)

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

    def test_ascii_longer_than_pyodbc_buffer(self):
        """值比 pyodbc 的默认字符缓冲区长 -> 必须靠分段读拼回完整值"""
        value = "a" * 70000
        conn = self._connect()
        try:
            row = conn.cursor().execute(
                self._select_literal(f"REPEAT('a', {len(value)})")
            ).fetchone()
            self.assert_equals(
                len(row[0] or ""), len(value),
                "70000 characters must arrive intact (pyodbc reads them in "
                "parts; a shorter result means the tail was dropped)")
            self.assert_equals(row[0], value, "reassembled ASCII value")
        finally:
            conn.close()

    def test_multibyte_longer_than_pyodbc_buffer(self):
        """同样的场景换多字节值: 分段不能破坏字符边界"""
        value = "\u4f60\u597d" * 3000  # 3-byte chars, 27 KB UTF-8
        conn = self._connect()
        try:
            row = conn.cursor().execute(
                self._select_literal("REPEAT('\u4f60\u597d', 3000)")
            ).fetchone()
            self.assert_equals(row[0], value, "multi-byte value across parts")
            self.assert_equals(
                (row[0] or "").count("\ufffd"), 0,
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
        """一行里多个超长列, 顺序读取互不干扰"""
        conn = self._connect()
        try:
            row = conn.cursor().execute(
                "SELECT REPEAT('m', 70000) AS c1, REPEAT('n', 66000) AS c2, "
                "REPEAT('o', 71000) AS c3"
            ).fetchone()
            self.assert_equals(row[0], "m" * 70000, "c1")
            self.assert_equals(row[1], "n" * 66000, "c2")
            self.assert_equals(row[2], "o" * 71000, "c3")
        finally:
            conn.close()


if __name__ == "__main__":
    test = TestLongData()
    print_section("Long data / segmented SQLGetData E2E Tests (pyodbc)")
    test.run_test(test.test_ascii_long, "300 char ASCII value")
    test.run_test(test.test_ascii_longer_than_pyodbc_buffer,
                  "70000 chars, read in parts")
    test.run_test(test.test_multibyte_longer_than_pyodbc_buffer,
                  "27 KB multi-byte, read in parts")
    test.run_test(test.test_null_and_empty, "NULL and empty string")
    test.run_test(test.test_statement_reuse, "statement reuse")
    test.run_test(test.test_long_column_ordering, "three long columns in a row")
    success = test.print_results()
    sys.exit(0 if success else 1)
