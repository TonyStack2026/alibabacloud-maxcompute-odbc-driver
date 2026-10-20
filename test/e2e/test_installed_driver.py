"""Installed Windows package smoke test; does not contact a database."""
import faulthandler
import os

faulthandler.enable()
os.environ["MCO_LOG_LEVEL"] = "DEBUG"  # Only synthetic local probe credentials.

import pyodbc

pyodbc.pooling = False
assert "MaxCompute ODBC Driver" in pyodbc.drivers(), "MSI did not register the driver"

# Use the W APIs explicitly: pyodbc.getinfo calls the ANSI API, so its Chinese
# strings can be down-converted by the Windows Driver Manager's system code page.
import ctypes as C


def verify_unicode_driver_manager_path():
    odbc = C.WinDLL("odbc32")
    handle, short, ushort, integer = C.c_void_p, C.c_short, C.c_ushort, C.c_int
    odbc.SQLAllocHandle.argtypes = [short, handle, C.POINTER(handle)]
    odbc.SQLSetEnvAttr.argtypes = [handle, integer, handle, integer]
    odbc.SQLDriverConnectW.argtypes = [handle, handle, C.c_wchar_p, short, handle, short, C.POINTER(short), ushort]
    odbc.SQLGetInfoW.argtypes = [handle, ushort, handle, short, C.POINTER(short)]
    odbc.SQLDisconnect.argtypes = [handle]
    odbc.SQLFreeHandle.argtypes = [short, handle]
    for name in ("SQLAllocHandle", "SQLSetEnvAttr", "SQLDriverConnectW", "SQLGetInfoW", "SQLDisconnect", "SQLFreeHandle"):
        getattr(odbc, name).restype = short
    env, dbc = handle(), handle()
    connected = False
    def check(result, operation):
        assert result in (0, 1), f"{operation}: ODBC status {result}"
    try:
        check(odbc.SQLAllocHandle(1, None, C.byref(env)), "Allocate environment")
        check(odbc.SQLSetEnvAttr(env, 200, handle(3), 0), "Set ODBC version")
        check(odbc.SQLAllocHandle(2, env, C.byref(dbc)), "Allocate connection")
        print("Connecting Unicode probe through Windows Driver Manager", flush=True)
        check(odbc.SQLDriverConnectW(
            dbc, None,
            'Driver={MaxCompute ODBC Driver};Endpoint=http://127.0.0.1:9/api;'
            'Project="中文探针";ClientCharset={GBK};UID=package_probe;PWD=package_probe;',
            -3, None, 0, None, 0), "Unicode connection")
        connected = True
        for info, expected in ((7, "1.0.1"), (16, "中文探针"), (17, "MaxCompute")):
            output, length = C.create_unicode_buffer(128), short()
            check(odbc.SQLGetInfoW(dbc, info, output, C.sizeof(output), C.byref(length)), "Unicode metadata")
            assert output.value == expected, (info, ascii(output.value))
            assert length.value == len(expected.encode("utf-16-le")), (info, length.value)
        print("Installed Unicode driver: Windows W connection and Chinese metadata OK", flush=True)
    finally:
        if connected:
            odbc.SQLDisconnect(dbc)
        if dbc:
            odbc.SQLFreeHandle(2, dbc)
        if env:
            odbc.SQLFreeHandle(1, env)


verify_unicode_driver_manager_path()

for options in (
    'Project="package_probe";ClientCharset=GBK;',
    'Project=package_probe;ClientCharset={GBK};',
):
    print("Connecting installed package; Unicode project=" + str("中文探针" in options), flush=True)
    with pyodbc.connect(
        "Driver={MaxCompute ODBC Driver};Endpoint=http://127.0.0.1:9/api;"
        "UID=package_probe;PWD=package_probe;" + options,
        timeout=5,
        autocommit=True,
    ) as connection:
        print("Connection completed; reading metadata", flush=True)
        version = connection.getinfo(pyodbc.SQL_DRIVER_VER)
        assert version == "1.0.1", f"Unexpected installed driver: {version}"
        assert connection.getinfo(pyodbc.SQL_DBMS_NAME) == "MaxCompute"
        assert connection.getinfo(pyodbc.SQL_DATABASE_NAME) == "package_probe"
        print(f"Installed ODBC driver {version}: connection and metadata OK")
