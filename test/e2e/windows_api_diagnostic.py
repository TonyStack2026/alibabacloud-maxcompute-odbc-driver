"""Isolate native statement allocation/type-info calls with synthetic settings."""
import ctypes as C
import faulthandler
import os
from pathlib import Path
import sys

faulthandler.enable()
os.environ['MCO_LOG_LEVEL'] = 'DEBUG'
mode = sys.argv[1]
path = 'odbc32' if mode == 'manager' else str(Path(os.environ['SystemRoot']) / 'System32/maxcompute_odbc.dll')
api = C.WinDLL(path)
H, S, U, I = C.c_void_p, C.c_short, C.c_ushort, C.c_int
functions = {
    'SQLAllocHandle': [S, H, C.POINTER(H)],
    'SQLSetEnvAttr': [H, I, H, I],
    'SQLDriverConnectW': [H, H, C.c_wchar_p, S, H, S, C.POINTER(S), U],
    'SQLGetStmtAttrW': [H, I, H, I, C.POINTER(I)],
    'SQLGetTypeInfoW': [H, S],
    'SQLGetTypeInfo': [H, S],
    'SQLFreeStmt': [H, U],
    'SQLDisconnect': [H],
    'SQLFreeHandle': [S, H],
}
for name, arguments in functions.items():
    try:
        function = getattr(api, name)
        function.argtypes, function.restype = arguments, S
        print(f'{mode}: export {name} present', flush=True)
    except AttributeError:
        print(f'{mode}: export {name} MISSING', flush=True)
env, dbc, stmt = H(), H(), H()
assert api.SQLAllocHandle(1, None, C.byref(env)) == 0
assert api.SQLSetEnvAttr(env, 200, H(3), 0) == 0
assert api.SQLAllocHandle(2, env, C.byref(dbc)) == 0
text = 'Driver={MaxCompute ODBC Driver};Endpoint=http://127.0.0.1:9/api;Project=probe;UID=probe;PWD=probe;'
assert api.SQLDriverConnectW(dbc, None, text, -3, None, 0, None, 0) in (0, 1)
print(f'{mode}: BEFORE statement allocation', flush=True)
status = api.SQLAllocHandle(3, dbc, C.byref(stmt))
print(f'{mode}: AFTER statement allocation status={status}', flush=True)
assert status in (0, 1)
for attribute in (6, 10010):
    if hasattr(api, 'SQLGetStmtAttrW'):
        output = H()
        print(f'{mode}: BEFORE statement attribute {attribute}', flush=True)
        status = api.SQLGetStmtAttrW(stmt, attribute, C.byref(output), C.sizeof(output), None)
        print(f'{mode}: AFTER statement attribute status={status}', flush=True)
if hasattr(api, 'SQLGetTypeInfoW'):
    print(f'{mode}: BEFORE type information', flush=True)
    status = api.SQLGetTypeInfoW(stmt, 12)
    print(f'{mode}: AFTER type information status={status}', flush=True)
print(f'{mode}: BEFORE statement close', flush=True)
print(api.SQLFreeStmt(stmt, 0), flush=True)
print(api.SQLFreeHandle(3, stmt), flush=True)
print(api.SQLDisconnect(dbc), flush=True)
print(api.SQLFreeHandle(2, dbc), flush=True)
print(api.SQLFreeHandle(1, env), flush=True)
