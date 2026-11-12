"""Exercise the actual C ring parser/forwarder as three independent native nodes."""
import binascii
import ctypes
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def crc_frame(src, dst, seq, ttl, payload):
    data = bytes((0xA5, 0x5A, 1, src, dst, ttl, seq, len(payload))) + payload
    return data + binascii.crc_hqx(data, 0xFFFF).to_bytes(2, "big")


class Node:
    def __init__(self, path):
        self.lib = ctypes.CDLL(str(path))
        self.lib.send_frame.argtypes = [ctypes.c_uint] * 3 + [ctypes.c_void_p, ctypes.c_uint]
        self.lib.feed.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        self.reset()

    def reset(self):
        self.lib.reset_node()

    def feed(self, data):
        self.lib.feed(data, len(data))

    def take(self, method):
        out = ctypes.create_string_buffer(128)
        n = getattr(self.lib, method)(out)
        return out.raw[:n]


def main():
    with tempfile.TemporaryDirectory(prefix="header-ring-") as directory:
        nodes = []
        for node in range(1, 4):
            path = Path(directory) / f"node{node}.so"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                            f"-DHEADER_RING_NODE_ID={node}", str(ROOT / "tools/header_ring_host.c"),
                            "-o", str(path)], check=True)
            nodes.append(Node(path))
        payload = bytes.fromhex("0055FFAAA55A0102030405060708090A")
        assert not nodes[0].lib.send_frame(2, 1, 4, payload+b"x", 17)
        # All six directed paths plus all three complete circuits back to the origin.
        for src in range(3):
            for dst in range(3):
                for node in nodes:
                    node.reset()
                assert nodes[src].lib.send_frame(dst+1, 77, 4, payload, len(payload))
                frame = nodes[src].take("take_tx")
                assert frame == crc_frame(src+1, dst+1, 77, 4, payload)
                hops = (dst-src) % 3 or 3
                for hop in range(1, hops+1):
                    receiver = nodes[(src+hop) % 3]
                    receiver.feed(frame)
                    if hop == hops:
                        assert receiver.take("take_last") == crc_frame(src+1, dst+1, 77, 5-hop, payload)
                        assert receiver.take("take_tx") == b""
                    else:
                        frame = receiver.take("take_tx")
                        expected = crc_frame(src+1, dst+1, 77, 4-hop, payload)
                        assert frame == expected, (src, dst, hop, frame.hex(), expected.hex())
        n = nodes[0]
        n.reset()
        bad = bytearray(crc_frame(2, 1, 1, 4, payload)); bad[-1] ^= 1
        n.feed(bytes(bad))
        assert n.lib.counter(3) == 1 and n.take("take_last") == b"" and n.take("take_tx") == b""
        n.feed(b"noise\xa5\xa5\x5a\x00\x02\x01\x04\x01\x01")
        assert n.lib.counter(4) == 1
        n.feed(b"\xa5\x5a\x01")
        n.lib.advance(2001)
        assert n.lib.counter(4) == 2
        n.feed(crc_frame(2, 1, 2, 4, payload))
        assert n.take("take_last") == crc_frame(2, 1, 2, 4, payload)
        # Unknown destinations stop after their hop budget.
        for node in nodes:
            node.reset()
        frame = crc_frame(1, 254, 9, 3, payload)
        for idx in (1, 2, 0):
            nodes[idx].feed(frame)
            frame = nodes[idx].take("take_tx")
        assert frame == b"" and nodes[0].lib.counter(2) == 1
        # Full TX queue defers a whole frame, without forwarding partial data.
        n.reset()
        packets = [crc_frame(2, 3, seq, 4, payload) for seq in (1, 2, 3)]
        for packet in packets:
            n.feed(packet)
        assert n.lib.counter(5) == 26
        assert n.take("take_tx") == b"".join(crc_frame(2, 3, seq, 3, payload) for seq in (1, 2))
        assert n.take("take_tx") == crc_frame(2, 3, 3, 3, payload)
        assert n.lib.counter(1) == 3 and n.lib.counter(5) == 0
    print("PASS: all directed routes and full circuits; CRC rejection; parser recovery; TTL; backpressure")


if __name__ == "__main__":
    main()
