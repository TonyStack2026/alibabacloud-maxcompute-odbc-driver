"""Installed Windows package smoke test; does not contact a database."""
import pyodbc

pyodbc.pooling = False
assert "MaxCompute ODBC Driver" in pyodbc.drivers(), "MSI did not register the driver"

for options in (
    'Project="package_probe";ClientCharset=GBK;',
    'Project=package_probe;ClientCharset={GBK};',
    'Project="中文探针";ClientCharset={GBK};',
):
    with pyodbc.connect(
        "Driver={MaxCompute ODBC Driver};Endpoint=http://127.0.0.1:9/api;"
        "UID=package_probe;PWD=package_probe;" + options,
        timeout=5,
    ) as connection:
        version = connection.getinfo(pyodbc.SQL_DRIVER_VER)
        assert version == "1.0.1", f"Unexpected installed driver: {version}"
        assert connection.getinfo(pyodbc.SQL_DBMS_NAME) == "MaxCompute"
        expected_project = "中文探针" if "中文探针" in options else "package_probe"
        assert connection.getinfo(pyodbc.SQL_DATABASE_NAME) == expected_project
        print(f"Installed ODBC driver {version}: connection and metadata OK")
