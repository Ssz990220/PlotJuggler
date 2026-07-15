# SPDX-License-Identifier: MPL-2.0
"""Package test for the PJ4 LGPL-lean FFmpeg: asserts the built libraries are
LGPL-only and that the .pc metadata relocated into the installed prefix.

Fails the package build if the configure line ever regresses to GPL/nonfree,
so a license slip is caught at packaging time, not at a release audit.
Runs on linux-64 and win-64.
"""

import ctypes
import glob
import os
import subprocess
import sys

PREFIX = os.environ.get("PREFIX") or sys.prefix


def load_avcodec() -> ctypes.CDLL:
    if os.name == "nt":
        bin_dir = os.path.join(PREFIX, "Library", "bin")
        candidates = glob.glob(os.path.join(bin_dir, "avcodec-*.dll"))
        assert len(candidates) == 1, f"expected exactly one avcodec DLL, got {candidates}"
        # Required for ctypes to resolve the DLL's own deps (zlib, dav1d) from
        # the same directory; the cookie must stay alive while we call into it.
        global _dll_dir_cookie
        _dll_dir_cookie = os.add_dll_directory(bin_dir)
        return ctypes.CDLL(candidates[0])
    candidates = sorted(glob.glob(os.path.join(PREFIX, "lib", "libavcodec.so.*.*.*")))
    assert candidates, f"no libavcodec shared library under {PREFIX}/lib"
    return ctypes.CDLL(candidates[0])


def check_license(lib: ctypes.CDLL) -> None:
    lib.avcodec_license.restype = ctypes.c_char_p
    lib.avcodec_configuration.restype = ctypes.c_char_p
    license_text = lib.avcodec_license().decode()
    configuration = lib.avcodec_configuration().decode()
    print(f"avcodec license: {license_text}")
    print(f"avcodec configuration: {configuration}")
    assert "LGPL" in license_text, "avcodec_license() does not report LGPL"
    assert "--enable-gpl" not in configuration, "configuration contains --enable-gpl"
    assert "--enable-nonfree" not in configuration, "configuration contains --enable-nonfree"
    assert "--disable-gpl" in configuration, "configuration is missing --disable-gpl"


def check_pkgconfig_relocation() -> None:
    """The .pc files must resolve inside THIS installed prefix — catches a
    failed conda prefix relocation (stale build-time placeholder paths)."""
    if os.name == "nt":
        pkgconfig_dir = os.path.join(PREFIX, "Library", "lib", "pkgconfig")
    else:
        pkgconfig_dir = os.path.join(PREFIX, "lib", "pkgconfig")
    env = {**os.environ, "PKG_CONFIG_PATH": pkgconfig_dir}
    for module in ("libavcodec", "libavformat", "libavutil", "libswscale"):
        subprocess.run(["pkg-config", "--exists", module], check=True, env=env)
        cflags = subprocess.run(
            ["pkg-config", "--cflags", module], check=True, capture_output=True, text=True, env=env
        ).stdout
        normalized = cflags.replace("\\", "/").lower()
        prefix_normalized = PREFIX.replace("\\", "/").lower()
        assert prefix_normalized in normalized, (
            f"{module}: pkg-config cflags do not point into the installed prefix\n"
            f"  cflags: {cflags!r}\n  prefix: {PREFIX!r}"
        )


def main() -> None:
    check_license(load_avcodec())
    check_pkgconfig_relocation()
    print("license guard: OK")


if __name__ == "__main__":
    main()
