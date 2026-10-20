"""Installed Windows package smoke test; does not contact a database."""
import faulthandler
import os

faulthandler.enable()
os.environ["MCO_LOG_LEVEL"] = "DEBUG"  # Only synthetic local probe credentials.

import pyodbc

pyodbc.pooling = False
assert "MaxCompute ODBC Driver" in pyodbc.drivers(), "MSI did not register the driver"

for options in (
    'Project="package_probe";ClientCharset=GBK;',
    'Project=package_probe;ClientCharset={GBK};',
    'Project="中文探针";ClientCharset={GBK};',
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
        expected_project = "中文探针" if "中文探针" in options else "package_probe"
        assert connection.getinfo(pyodbc.SQL_DATABASE_NAME) == expected_project
        print(f"Installed ODBC driver {version}: connection and metadata OK")
