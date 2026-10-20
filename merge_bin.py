# MantisCore merged image: bootloader (0x1000) + partition table (0x8000) + app (0x10000)
# -> mantiscore-merged.bin, flashed at 0x0. Fails the build if the app outgrows its 1.5 MB slot.
# Same esptool invocation that Launcher uses with this platform.
Import("env")
import os, subprocess
from pathlib import Path

build_dir = Path(env.subst("$BUILD_DIR"))
proj_dir = Path(env.subst("$PROJECT_DIR"))
boot_bin, part_bin, app_bin = build_dir / "bootloader.bin", build_dir / "partitions.bin", build_dir / "firmware.bin"
out_bin = proj_dir / "mantiscore-merged.bin"
APP_SLOT = 0x180000  # app0 (test) in partitions.csv
esptool_pkg = env.PioPlatform().get_package_dir("tool-esptoolpy")

def merge(target, source, env):
    missing = [p for p in (boot_bin, part_bin, app_bin) if not p.exists()]
    if missing:
        print("[merge] missing:", ", ".join(map(str, missing)))
        env.Exit(1)
    size = app_bin.stat().st_size
    print(f"[merge] MantisCore app 0x{size:X} of 0x{APP_SLOT:X} ({size * 100 / APP_SLOT:.1f}%)")
    if size > APP_SLOT:
        print("[merge] ERROR: firmware.bin is larger than the app0 partition")
        env.Exit(1)
    cmd = [env.get("PYTHONEXE", "python"), "-m", "esptool", "--chip", "esp32", "merge-bin",
           "--output", str(out_bin), "0x1000", str(boot_bin), "0x8000", str(part_bin), "0x10000", str(app_bin)]
    menv = os.environ.copy()
    menv["PYTHONPATH"] = str(esptool_pkg)
    r = subprocess.run(cmd, env=menv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        print(r.stderr.decode(errors="replace"))
        env.Exit(r.returncode)
    print(f"[merge] OK {out_bin} ({out_bin.stat().st_size} bytes)")

env.AddPostAction(str(app_bin), merge)
