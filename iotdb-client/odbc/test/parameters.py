# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""Check parameter values at the REST boundary using the public ODBC ABI."""

import ctypes as c
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
import sys
import threading


class Timestamp(c.Structure):
    _fields_ = [
        (name, c.c_short)
        for name in ("year", "month", "day", "hour", "minute", "second")
    ] + [("fraction", c.c_uint)]


def main():
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((self.path, payload["sql"]))
            response = b'{"code":200,"expressions":null,"column_names":[],"values":[]}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        def log_message(self, *_):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    lib = (c.WinDLL if os.name == "nt" else c.CDLL)(os.path.abspath(sys.argv[1]))
    handle, small, integer, length = c.c_void_p, c.c_short, c.c_int, c.c_ssize_t

    def api(name, *args):
        function = getattr(lib, name)
        function.argtypes = args
        function.restype = small
        return function

    alloc = api("SQLAllocHandle", small, handle, c.POINTER(handle))
    free = api("SQLFreeHandle", small, handle)
    connect = api(
        "SQLDriverConnect",
        handle,
        handle,
        c.c_char_p,
        small,
        handle,
        small,
        handle,
        c.c_ushort,
    )
    prepare = api("SQLPrepare", handle, c.c_char_p, integer)
    bind = api(
        "SQLBindParameter",
        handle,
        c.c_ushort,
        small,
        small,
        small,
        c.c_size_t,
        small,
        handle,
        length,
        c.POINTER(length),
    )
    execute = api("SQLExecute", handle)
    param_data = api("SQLParamData", handle, c.POINTER(handle))
    put = api("SQLPutData", handle, handle, length)
    direct = api("SQLExecDirect", handle, c.c_char_p, integer)
    get_attr = api("SQLGetStmtAttr", handle, integer, handle, integer, handle)
    set_desc = api("SQLSetDescField", handle, small, small, handle, integer)
    env, dbc, stmt = handle(), handle(), handle()
    try:
        assert alloc(1, None, c.byref(env)) == 0
        assert alloc(2, env, c.byref(dbc)) == 0
        options = (
            f"SERVER=127.0.0.1;PORT={server.server_port};"
            "RESTFUL=1;ISTABLEMODEL=0;UID=root;PWD=root;"
        ).encode()
        assert connect(dbc, None, options, -3, None, 0, None, 0) == 0
        assert alloc(3, dbc, c.byref(stmt)) == 0

        def check(value, ctype, sqltype, expected, streamed=False, indicator=None):
            assert prepare(stmt, b"insert into root.d(time, v) values(1, ?)", -3) == 0
            ind = (
                length(-2 if streamed else indicator)
                if streamed or indicator is not None
                else None
            )
            assert (
                bind(
                    stmt,
                    1,
                    1,
                    ctype,
                    sqltype,
                    64,
                    0,
                    c.byref(value),
                    c.sizeof(value),
                    c.byref(ind) if ind is not None else None,
                )
                == 0
            )
            if streamed:
                assert execute(stmt) == 99
                token = handle()
                assert param_data(stmt, c.byref(token)) == 99
                assert token.value == c.addressof(value)
                assert (
                    put(
                        stmt,
                        c.byref(value),
                        indicator if indicator is not None else c.sizeof(value),
                    )
                    == 0
                )
                assert param_data(stmt, c.byref(token)) == 0
            else:
                assert execute(stmt) == 0
            assert requests[-1] == (
                "/rest/v2/nonQuery",
                f"insert into root.d(time, v) values(1, {expected})",
            ), requests[-1]

        check(c.c_double(1.2345678901234567), 8, 8, "1.2345678901234567")

        # Tail bytes make an incorrect wider read deterministic.
        class SmallBuffer(c.Structure):
            _fields_ = [("value", c.c_short), ("tail", c.c_short)]

        class TinyBuffer(c.Structure):
            _fields_ = [("value", c.c_byte), ("tail", c.c_byte * 3)]

        check(SmallBuffer(42, 1234), 99, 5, "42")
        check(TinyBuffer(42, (c.c_byte * 3)(1, 2, 3)), 99, -6, "42")
        check(c.create_string_buffer(b"12.3456"), 99, 3, "'12.3456'")
        check((c.c_ushort * 3)(0x4E2D, 0x6587, 0), 99, -9, "'中文'")
        check(
            Timestamp(2026, 9, 22, 1, 2, 3, 123456789),
            93,
            93,
            "'2026-09-22 01:02:03.123456789'",
        )
        check(c.create_string_buffer(b"it's text"), 99, 12, "'it''s text'")
        check(c.c_int(42), 4, 4, "42")
        check(c.c_int(42), 4, 4, "42", streamed=True)
        check((c.c_ubyte * 3)(0, 255, 39), -2, -3, "X'00FF27'", streamed=True)
        check(
            (c.c_ushort * 3)(0x4E2D, 0x6587, 0),
            -8,
            -9,
            "'中文'",
            streamed=True,
            indicator=-3,
        )
        # Explicit descriptor length and NULL-indicator pointers are independent.
        assert prepare(stmt, b"insert into root.d(time, v) values(1, ?)", -3) == 0
        value = c.create_string_buffer(b"abcdef")
        size, indicator, apd = length(3), length(0), handle()
        assert (
            bind(stmt, 1, 1, 1, 12, 64, 0, value, c.sizeof(value), c.byref(size)) == 0
        )
        assert get_attr(stmt, 10011, c.byref(apd), 0, None) == 0
        assert set_desc(apd, 1, 1009, c.byref(indicator), 0) == 0
        assert execute(stmt) == 0
        assert requests[-1][1].endswith("(1, 'abc')"), requests[-1]
        assert direct(stmt, b"/* comment */ SELECT 1", -3) == 0
        assert requests[-1][0] == "/rest/v2/query"
        print("ODBC parameter serialization checks passed")
    finally:
        for kind, value in ((3, stmt), (2, dbc), (1, env)):
            if value:
                free(kind, value)
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
