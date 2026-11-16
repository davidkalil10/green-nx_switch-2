import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from verify_switch2_package import (  # noqa: E402
    verify_aset,
    verify_link_map,
    verify_switch2_startup,
)


def make_fake_nro(
    *,
    with_oun0: bool = True,
    platform: int = 2,
    version: int = 1,
    with_lny: bool = True,
    with_aset: bool = True,
) -> bytes:
    buf = bytearray(0x4800)
    struct.pack_into("<I", buf, 4, 0x80)  # MOD0 offset
    buf[0x10:0x14] = b"NRO0"
    struct.pack_into("<I", buf, 0x18, 0x1000)  # native size / ASET start
    buf[0x80:0x84] = b"MOD0"
    at = 0x80 + 0x1C
    if with_lny:
        buf[at : at + 4] = b"LNY0"
        at += 12
    if with_oun0:
        buf[at : at + 4] = b"OUN0"
        struct.pack_into("<II", buf, at + 4, platform, version)

    if with_aset:
        aset = 0x1000
        buf[aset : aset + 4] = b"ASET"
        # icon (JPEG) at +0x40
        icon_off = 0x40
        icon_size = 64
        buf[aset + icon_off : aset + icon_off + 3] = b"\xff\xd8\xff"
        struct.pack_into("<QQ", buf, aset + 0x08, icon_off, icon_size)
        # nacp (0x4000) at +0x80
        nacp_off = 0x80
        nacp_size = 0x4000
        buf.extend(b"\x00" * (aset + nacp_off + nacp_size - len(buf)))
        struct.pack_into("<QQ", buf, aset + 0x18, nacp_off, nacp_size)
    return bytes(buf)


class VerifySwitch2PackageTests(unittest.TestCase):
    def test_valid_nro_and_link_map(self) -> None:
        raw = make_fake_nro()
        verify_switch2_startup(raw)
        verify_aset(raw)
        verify_link_map(
            "/opt/devkitpro/libounce/lib/libnx.a\n"
            "/opt/devkitpro/portlibs/switch2/lib/libEGL.a\n"
            "/opt/devkitpro/portlibs/switch/lib/libSDL2.a\n"
        )

    def test_rejects_missing_oun0(self) -> None:
        raw = make_fake_nro(with_oun0=False)
        with self.assertRaises(ValueError):
            verify_switch2_startup(raw)

    def test_rejects_wrong_platform(self) -> None:
        raw = make_fake_nro(platform=1)
        with self.assertRaises(ValueError):
            verify_switch2_startup(raw)

    def test_rejects_switch1_paths_in_link_map(self) -> None:
        for bad in (
            "/opt/devkitpro/libnx/lib/libnx.a",
            "/opt/devkitpro/portlibs/switch/lib/libEGL.a",
            "/opt/devkitpro/portlibs/switch/lib/libdeko3d.a",
            "/opt/devkitpro/portlibs/switch/lib/libavcodec.a",
        ):
            with self.assertRaises(ValueError):
                verify_link_map(
                    "/opt/devkitpro/libounce/lib/libnx.a\n"
                    "/opt/devkitpro/portlibs/switch2/lib/libEGL.a\n" + bad
                )


if __name__ == "__main__":
    unittest.main()
