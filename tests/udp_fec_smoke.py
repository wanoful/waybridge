#!/usr/bin/env python3
"""Compare XOR and Reed–Solomon through the real Windows UDP receiver under Xvfb."""
import io
import os
import struct
import subprocess
import tempfile
from pathlib import Path
from PIL import Image

from udp_flow_smoke import BUILD, ROOT, Crypto, DelayProxy, LossProxy, build, server, stop


class BurstLoss(LossProxy):
    def __init__(self, peer, relay, crypto):
        self.parity = set()
        super().__init__(peer, relay, crypto)

    def should_drop(self, plaintext, message):
        if message != 1:
            return False
        shard = int.from_bytes(plaintext[2:4], "big")
        if plaintext[0] == 2 and shard < 16:
            self.parity.add((shard, plaintext[6]))
        return plaintext[0] == 1 and shard in (1, 2)


def exercise(wine, crypto, use_rs, log):
    with server(120, log) as (runtime, environment, token, tcp_port, udp_port):
        relay = DelayProxy(("127.0.0.1", tcp_port))
        proxy = BurstLoss(("127.0.0.1", udp_port), relay, crypto)
        app = None
        try:
            app = subprocess.Popen(
                [str(BUILD / "udp-noise-fixture"), "160", "120"],
                env=environment, stdout=log, stderr=log,
            )
            env = {**wine, "WAYBRIDGE_UDP_TEST_WINDOWS": "1", "WAYBRIDGE_UDP_TEST_NO_REFRESH": "1",
                   "WAYBRIDGE_UDP_HOST": "127.0.0.1", "WAYBRIDGE_UDP_PORT": str(proxy.port)}
            env.pop("WAYBRIDGE_UDP_TEST_RS", None)
            if use_rs:
                env["WAYBRIDGE_UDP_TEST_RS"] = "1"
            output = runtime / "frame.bin"
            result = subprocess.run([
                "wine", str(ROOT / "build/windows/waybridge-udp-probe.exe"),
                str(relay.port), token, str(output), "1",
            ], env=env, capture_output=True, encoding="utf-8", errors="replace", timeout=20)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert relay.offer["version"] == 2
            assert relay.offer.get("fec", "xor") == ("rs" if use_rs else "xor"), relay.offer
            assert relay.offer["group"] == (16 if use_rs else 8)
            assert proxy.dropped == 2 and len(proxy.parity) == 2, (proxy.dropped, proxy.parity)
            assert not relay.errors and not proxy.errors, (relay.errors, proxy.errors)
            assert ("Solomon shards recovered" in result.stdout) == use_rs, result.stdout
            assert bool(relay.tcp_media) != use_rs, (relay.tcp_media, result.stdout)
            packet = output.read_bytes()
            size, = struct.unpack(">I", packet[:4])
            assert 36 < size == len(packet) - 4 and packet[4] == 1
            size = struct.unpack(">II", packet[9:17])
            image = Image.open(io.BytesIO(packet[41:])).convert("RGBA")
            assert image.size == size and size[0] >= 160 and size[1] >= 120, size
            return image.tobytes()
        finally:
            proxy.close()
            relay.close()
            if app:
                stop(app)


def main():
    build()
    subprocess.run(["cmake", "-S", "client", "-B", "build/windows", "-G", "Ninja",
                    "-DCMAKE_TOOLCHAIN_FILE=mingw.cmake", "-DCMAKE_BUILD_TYPE=Release"], cwd=ROOT, check=True)
    subprocess.run(["cmake", "--build", "build/windows", "--target", "waybridge-udp-probe"], cwd=ROOT, check=True)
    crypto = Crypto()
    with tempfile.TemporaryDirectory(prefix="waybridge-fec-") as directory:
        wine = {**os.environ, "WINEPREFIX": str(Path(directory) / "wine"), "WINEDEBUG": "-all",
                "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml="}
        with (BUILD / "udp-fec.log").open("w") as log:
            try:
                subprocess.run(["wineboot", "-u"], env=wine, stdout=log, stderr=log,
                               check=True, timeout=60)
                xor = exercise(wine, crypto, False, log)
                rs = exercise(wine, crypto, True, log)
                assert rs == xor, "reconstructed PNG differs from the refreshed reference"
            finally:
                subprocess.run(["wineserver", "-k"], env=wine, stdout=log, stderr=log)
    print("PASS: equal parity overhead, two lost shards, Reed–Solomon restored the original PNG without TCP repair")


if __name__ == "__main__":
    main()
