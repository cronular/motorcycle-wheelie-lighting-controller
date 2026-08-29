Import("env")

import os


toolchain_dir = env.PioPlatform().get_package_dir("toolchain-gccmingw32")
if not toolchain_dir:
    raise RuntimeError("PlatformIO MinGW package is not installed")

bin_dir = os.path.join(toolchain_dir, "bin")
env.PrependENVPath("PATH", bin_dir)
env.Replace(
    CC=os.path.join(bin_dir, "gcc.exe"),
    CXX=os.path.join(bin_dir, "g++.exe"),
    AR=os.path.join(bin_dir, "ar.exe"),
    RANLIB=os.path.join(bin_dir, "ranlib.exe"),
)
env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
