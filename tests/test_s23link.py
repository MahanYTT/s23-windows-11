"""s23link.py: open-retry/backoff, timeout guards, Link framing and CLI argument handling (no hardware)."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import s23link  # noqa: E402


class FakeClock:
    def __init__(self):
        self.t = 0.0
        self.sleeps = []

    def clock(self):
        return self.t

    def sleep(self, s):
        self.sleeps.append(round(s, 6))
        self.t += s


class RetryTransientTest(unittest.TestCase):
    def test_success_first_try_does_not_sleep(self):
        fc = FakeClock()
        self.assertEqual(s23link.retry_transient(lambda: 42, 5, fc.sleep, fc.clock), 42)
        self.assertEqual(fc.sleeps, [])

    def test_access_denied_is_retried_with_backoff_then_succeeds(self):
        fc = FakeClock()
        calls = []

        def fn():
            calls.append(fc.t)
            if len(calls) < 4:
                raise s23link.TransientOpenError('CreateFile failed 5', 5)
            return 'open'

        self.assertEqual(s23link.retry_transient(fn, 5, fc.sleep, fc.clock), 'open')
        self.assertEqual(fc.sleeps, [0.1, 0.2, 0.4])

    def test_backoff_is_capped_and_gives_up_at_deadline(self):
        fc = FakeClock()

        def fn():
            raise s23link.TransientOpenError('CreateFile failed 5', 5)

        with self.assertRaises(s23link.RemoteError) as cm:
            s23link.retry_transient(fn, 3, fc.sleep, fc.clock)
        self.assertNotIsInstance(cm.exception, s23link.TransientOpenError)
        self.assertIn('CreateFile failed 5', str(cm.exception))
        self.assertIn('gave up', str(cm.exception))
        self.assertLessEqual(max(fc.sleeps), 1.0)
        self.assertAlmostEqual(fc.t, 3.0)               # never sleeps past the deadline

    def test_zero_timeout_means_single_attempt(self):
        fc = FakeClock()
        n = []

        def fn():
            n.append(1)
            raise s23link.TransientOpenError('busy', 5)

        with self.assertRaises(s23link.RemoteError):
            s23link.retry_transient(fn, 0, fc.sleep, fc.clock)
        self.assertEqual(len(n), 1)
        self.assertEqual(fc.sleeps, [])

    def test_non_transient_errors_are_not_retried(self):
        fc = FakeClock()
        n = []

        def fn():
            n.append(1)
            raise s23link.RemoteError('RM9 USB device not found')

        with self.assertRaises(s23link.RemoteError):
            s23link.retry_transient(fn, 5, fc.sleep, fc.clock)
        self.assertEqual(len(n), 1)

    def test_access_denied_is_classified_transient(self):
        self.assertIn(s23link.ERROR_ACCESS_DENIED, s23link.TRANSIENT_OPEN_ERRORS)
        self.assertIn(s23link.ERROR_SHARING_VIOLATION, s23link.TRANSIENT_OPEN_ERRORS)


class TimeoutHelpersTest(unittest.TestCase):
    def test_timeout_ms_never_zero(self):
        # PIPE_TRANSFER_TIMEOUT = 0 means "infinite" in WinUSB
        self.assertEqual(s23link._timeout_ms(0), 1)
        self.assertEqual(s23link._timeout_ms(0.0004), 1)
        self.assertEqual(s23link._timeout_ms(-3), 1)
        self.assertEqual(s23link._timeout_ms(2.5), 2500)

    def test_open_timeout_env(self):
        with mock.patch.dict(os.environ, {'RM9_OPEN_TIMEOUT': '1.5'}):
            self.assertEqual(s23link.open_timeout(), 1.5)
        for bad in ('abc', '-1', ''):
            with mock.patch.dict(os.environ, {'RM9_OPEN_TIMEOUT': bad}):
                self.assertEqual(s23link.open_timeout(), s23link.OPEN_TIMEOUT_S)
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(s23link.open_timeout(), s23link.OPEN_TIMEOUT_S)


def frame(typ, rid, payload=b'', flags=0):
    return struct.pack('<HBBII', s23link.MAGIC, typ, flags, rid, len(payload)) + payload


class FakeTransport:
    """answers each request frame with responder(typ, rid, payload, flags) -> list of frames"""
    def __init__(self, responder):
        self.responder = responder
        self.sent = []
        self.queue = []

    def send(self, f):
        magic, typ, flags, rid, plen = struct.unpack_from('<HBBII', f, 0)
        payload = f[12:12 + plen]
        self.sent.append((typ, flags, rid, payload))
        self.queue.extend(self.responder(typ, rid, payload, flags))

    def recv(self, timeout_s):
        return self.queue.pop(0) if self.queue else None

    def close(self):
        pass


def make_link(responder):
    link = s23link.Link.__new__(s23link.Link)
    link.t = FakeTransport(responder)
    link.id = 100
    return link


class LinkProtocolTest(unittest.TestCase):
    def test_exec_collects_output_and_exit_code(self):
        def r(typ, rid, pl, flags):
            self.assertEqual(typ, 2)
            self.assertEqual(pl, b'30\nver')
            return [frame(0x82, rid - 1, b'stale'), frame(0x82, rid, b'Micro'), frame(0x82, rid, b'soft'),
                    frame(0x83, rid, struct.pack('<i', 3))]
        self.assertEqual(make_link(r).exec('ver', 30), (3, b'Microsoft'))

    def test_exec_truncated_exit_frame_raises(self):
        link = make_link(lambda typ, rid, pl, fl: [frame(0x83, rid, b'\x01')])
        with self.assertRaises(s23link.RemoteError):
            link.exec('ver', 1)

    def test_put_rejects_overlong_remote_path(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b'abc')
        self.addCleanup(os.unlink, f.name)
        link = make_link(lambda *a: [])
        with self.assertRaises(s23link.RemoteError) as cm:
            link.put(f.name, 'C:\\' + 'x' * s23link.MAXDATA)
        self.assertIn('too long', str(cm.exception))
        self.assertEqual(link.t.sent, [])

    def test_put_chunks_and_flags(self):
        data = os.urandom(40000)
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(data)
        self.addCleanup(os.unlink, f.name)
        link = make_link(lambda typ, rid, pl, fl: [frame(0x80, rid, struct.pack('<i', 0))])
        self.assertEqual(link.put(f.name, 'C:\\a.bin'), len(data))
        got = b''
        for i, (typ, flags, rid, pl) in enumerate(link.t.sent):
            self.assertEqual(typ, 3)
            self.assertEqual(flags, 1 if i == 0 else 0)
            plen = struct.unpack_from('<H', pl, 0)[0]
            self.assertEqual(pl[2:2 + plen], b'C:\\a.bin')
            self.assertLessEqual(len(pl), s23link.MAXDATA)
            got += pl[2 + plen:]
        self.assertEqual(got, data)

    def test_put_error_frame_keeps_full_message(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b'abc')
        self.addCleanup(os.unlink, f.name)
        link = make_link(lambda typ, rid, pl, fl: [frame(0x8F, rid, b'disk full')])
        with self.assertRaises(s23link.RemoteError) as cm:
            link.put(f.name, 'C:\\a.bin')
        self.assertIn('disk full', str(cm.exception))

    def test_get_writes_file_and_checks_size(self):
        def r(typ, rid, pl, fl):
            return [frame(0x84, rid, b'hello '), frame(0x84, rid, b'world'), frame(0x85, rid, struct.pack('<Q', 11))]
        out = os.path.join(tempfile.mkdtemp(), 'o.bin')
        self.assertEqual(make_link(r).get('C:\\x', out), 11)
        with open(out, 'rb') as f:
            self.assertEqual(f.read(), b'hello world')

    def test_get_size_mismatch_and_truncated_end(self):
        out = os.path.join(tempfile.mkdtemp(), 'o.bin')
        with self.assertRaises(s23link.RemoteError):
            make_link(lambda t, rid, p, f: [frame(0x84, rid, b'ab'), frame(0x85, rid, struct.pack('<Q', 3))]).get('x', out)
        with self.assertRaises(s23link.RemoteError):
            make_link(lambda t, rid, p, f: [frame(0x85, rid, b'\0\0')]).get('x', out)

    def test_ping_ignores_foreign_magic(self):
        def r(typ, rid, pl, fl):
            bad = bytearray(frame(0x81, rid, pl))
            bad[0] ^= 0xFF
            return [bytes(bad), b'short', frame(0x81, rid, pl)]
        self.assertGreaterEqual(make_link(r).ping(), 0)

    def test_recv_times_out(self):
        link = make_link(lambda *a: [])
        with mock.patch.object(s23link.time, 'time', side_effect=[0, 0, 100]):
            with self.assertRaises(s23link.RemoteError):
                link._recv(1, 1)


class CliTest(unittest.TestCase):
    def run_main(self, *argv):
        err, out = io.StringIO(), io.StringIO()
        with mock.patch.object(s23link, 'Link', side_effect=AssertionError('must not open the device')):
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                rc = s23link.main(list(argv))
        return rc, out.getvalue() + err.getvalue()

    def test_usage_errors_do_not_touch_the_device(self):
        for argv in ([], ['--tcp'], ['--tcp', 'x', 'ping'], ['bogus'], ['exec'], ['exec', '-t'], ['exec', '-t', 'x', 'ver'],
                     ['exec', '-t', '5'], ['put', 'only-one'], ['get', 'a'], ['put', 'missing-file', 'C:\\x']):
            rc, out = self.run_main(*argv)
            self.assertEqual(rc, 2, argv)
            self.assertIn('usage:', out)

    def test_help(self):
        rc, out = self.run_main('--help')
        self.assertEqual(rc, 2)
        self.assertIn('exec [-t SEC]', out)

    def test_remote_error_is_reported_with_exit_code_1(self):
        err = io.StringIO()
        with mock.patch.object(s23link, 'Link', side_effect=s23link.RemoteError('CreateFile failed 5 (gave up)')):
            with contextlib.redirect_stderr(err):
                rc = s23link.main(['ping'])
        self.assertEqual(rc, 1)
        self.assertIn('CreateFile failed 5', err.getvalue())

    def test_exec_passes_timeout_and_returns_remote_exit_code(self):
        fake = mock.MagicMock()
        fake.__enter__.return_value.exec.return_value = (7, b'out')
        with mock.patch.object(s23link, 'Link', return_value=fake):
            with contextlib.redirect_stdout(io.StringIO()):
                rc = s23link.main(['exec', '-t', '120', 'dir', 'C:\\'])
        self.assertEqual(rc, 7)
        fake.__enter__.return_value.exec.assert_called_once_with('dir C:\\', 120)


if __name__ == '__main__':
    unittest.main()
