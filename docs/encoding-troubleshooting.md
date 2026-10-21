# Diagnosing text encoding in Power BI

Version 1.0.1 fixes braced `ClientCharset` values and parameters following a
double quoted value. It also preserves `ClientCharset` in the completed
connection string returned by the driver.

It also exposes the Unicode connection entry point required by the Windows
Driver Manager and returns UTF-16 string metadata from `SQLGetInfoW`.

Version 1.0.2 additionally reports text columns as Unicode SQL types. This
allows applications that choose their fetch type from metadata to request
UTF-16 directly. An explicit `SQL_C_CHAR` request still uses `ClientCharset`;
the Driver Manager does not override it.

`ClientCharset` controls the bytes returned for `SQL_C_CHAR`. It does not
change MaxCompute data, SQL text encoding, or `SQL_C_WCHAR`, which uses UTF-16.

In Power BI, select **ODBC**, set the DSN selection to **None**, and supply the
complete connection string. Try the default encoding first. If your application
explicitly requests narrow text and expects GBK, set `ClientCharset=GBK`.
For diagnosis, first create `C:\Temp`, then add:

```text
Driver={MaxCompute ODBC Driver};Endpoint=<endpoint>;Project=<project>;ClientCharset=GBK;LogLevel=INFO;LogPath=C:\Temp\maxcompute-odbc.log;
```

Keep authentication in the existing Power BI credential configuration.
Restart Power BI and refresh the affected query once. Look for:

```text
[ODBC-Encoding] driver=1.0.2; ClientCharset=GBK; source=connection-string; Unicode=UTF-16
[ODBC-Encoding] statement=... path=SQLGetData column=1 target_type=SQL_C_CHAR(1) encoding=GBK
```

- `source=default` indicates that no nonempty `ClientCharset` reached that connection.
- `SQL_C_CHAR` uses the configured client encoding. It must match the encoding
  expected by the application. GBK is not appropriate for every Windows locale.
- `SQL_C_WCHAR(-8)` uses UTF-16 and is unaffected by `ClientCharset`.
- `SQL_C_BINARY(-2)` returns binary bytes.
- `Unsupported charset ... falling back to UTF-8 output` means that the requested
  encoding could not be used. Do not interpret the configured charset alone as
  proof of the bytes returned in this case.

The encoding lines contain configuration and ODBC types, without credentials,
SQL text, or cell values. The connection line is written once per connection;
read diagnostics are written once per result column and target type, rather
than once per row. `INFO` is sufficient; `DEBUG` is not required.

If text is still garbled, provide the encoding lines, the Power BI version,
whether values or column names are affected, and a non-sensitive expected/actual
example. If the same data is already garbled in another MaxCompute client,
investigate the stored data separately. Remove `LogPath` after diagnosis.
