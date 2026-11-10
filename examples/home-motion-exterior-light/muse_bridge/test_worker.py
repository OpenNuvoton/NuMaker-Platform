import concurrent.futures
import io
import json
import queue
import subprocess
import threading
import time
import unittest
from unittest.mock import Mock

from .executor import BridgeError, WindowsSerial
from .test_executor import STATUS


class Input:
    def __init__(self, callback):
        self.callback = callback

    def write(self, line):
        self.callback(json.loads(line))

    def flush(self):
        pass


class WorkerTests(unittest.TestCase):
    def worker(self, handler):
        transport = WindowsSerial()
        output = queue.Queue()
        transport._response_queue = output
        transport._start_worker = lambda: None
        transport._close_worker = Mock()
        process = Mock()
        process.stdin = Input(lambda request: handler(request, output))
        transport._process = process
        return transport, output

    def test_ready_handshake_and_multiple_requests_share_worker(self):
        seen = []
        def handler(request, output):
            seen.append(request)
            if len(seen) == 1:
                output.put({"ready": True, "pid": 1234})
            output.put({"id": request["id"], "ok": True, "responses": [STATUS]})
        transport, _ = self.worker(handler)
        for _ in range(3):
            self.assertTrue(transport.send(["STATUS"])["oled"]["detected"])
        self.assertEqual([r["id"] for r in seen], [1,2,3])
        self.assertEqual(transport._windows_pid, 1234)
        transport._close_worker.assert_not_called()

    def test_board_rejection_preserves_connection_for_next_command(self):
        def handler(request, output):
            if request["id"] == 1:
                output.put({"id": 1, "ok": False, "error": "Board rejected command: ERR invalid command"})
            else:
                output.put({"id": request["id"], "ok": True, "responses": [STATUS]})
        transport, _ = self.worker(handler)
        with self.assertRaisesRegex(BridgeError, "Board rejected command"):
            transport.send(["BUFFER 1024 aa"])
        self.assertTrue(transport.send(["STATUS"])["oled"]["on"])
        transport._close_worker.assert_not_called()

    def test_mismatched_request_is_not_accepted(self):
        def handler(request, output):
            output.put({"id": 999, "ok": True, "responses": [STATUS]})
        transport, _ = self.worker(handler)
        with self.assertRaisesRegex(BridgeError, "did not match"):
            transport.send(["STATUS"])
        transport._close_worker.assert_called_with(force=True)

    def test_disconnect_and_timeout_close_worker(self):
        transport, output = self.worker(lambda request, output: output.put({"_error": "disconnected"}))
        with self.assertRaisesRegex(BridgeError, "disconnected"):
            transport.send(["STATUS"])
        transport._close_worker.assert_called_with(force=True)
        transport, output = self.worker(lambda request, output: None)
        with self.assertRaisesRegex(BridgeError, "timed out"):
            transport.send(["STATUS"], timeout_ms=100)
        transport._close_worker.assert_called_with(force=True)

    def test_transport_serializes_concurrent_muse_invocations(self):
        active = 0
        max_active = 0
        state_lock = threading.Lock()
        def runner(args, **kwargs):
            nonlocal active, max_active
            with state_lock:
                active += 1
                max_active = max(active, max_active)
            time.sleep(0.01)
            with state_lock:
                active -= 1
            return subprocess.CompletedProcess(args, 0, json.dumps({"ok": True, "responses": [STATUS]}), "")
        transport = WindowsSerial(runner=runner)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: transport.send(["STATUS"]), range(8)))
        self.assertEqual(len(results), 8)
        self.assertEqual(max_active, 1)


if __name__ == "__main__":
    unittest.main()
