"""s23link TcpTransport framing against a local fake agent (127.0.0.1, no USB, no phone)."""
import os
import socket
import struct
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import s23link  # noqa: E402


def frame(typ, rid, payload=b''):
    return struct.pack('<HBBII', s23link.MAGIC, typ, 0, rid, len(payload)) + payload


class FakeAgent:
    """accepts one client; answers ping (1) with pong (0x81) and exec (2) with output + exit code,
    deliberately splitting replies into small TCP writes to exercise reassembly"""
    def __init__(self):
        self.srv = socket.socket()
        self.srv.bind(('127.0.0.1', 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        c, _ = self.srv.accept()
        buf = b''
        with c:
            while True:
                d = c.recv(65536)
                if not d:
                    return
                buf += d
                while len(buf) >= 12:
                    magic, typ, flags, rid, plen = struct.unpack_from('<HBBII', buf, 0)
                    if len(buf) < 12 + plen:
                        break
                    payload, buf = buf[12:12 + plen], buf[12 + plen:]
                    if typ == 1:
                        out = frame(0x81, rid, payload)
                    elif typ == 2:
                        out = frame(0x82, rid, b'hello ') + frame(0x82, rid, b'phone') + frame(0x83, rid, struct.pack('<i', 0))
                    else:
                        out = frame(0x8F, rid, b'unsupported')
                    for i in range(0, len(out), 5):       # dribble the bytes out
                        c.sendall(out[i:i + 5])

    def close(self):
        self.srv.close()


class TcpTransportTest(unittest.TestCase):
    def setUp(self):
        self.agent = FakeAgent()
        self.addCleanup(self.agent.close)
        self.link = s23link.Link(tcp=self.agent.port)
        self.addCleanup(self.link.close)

    def test_ping_roundtrip(self):
        self.assertGreaterEqual(self.link.ping(b'abc'), 0)

    def test_exec_reassembles_split_frames(self):
        self.assertEqual(self.link.exec('ver', 5), (0, b'hello phone'))

    def test_error_frame_raises(self):
        with self.assertRaises(s23link.RemoteError):
            self.link.get('C:\\nope', os.devnull)

    def test_recv_timeout_returns_none(self):
        self.assertIsNone(self.link.t.recv(0.05))


if __name__ == '__main__':
    unittest.main()
