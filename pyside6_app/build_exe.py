"""
LH001-91 心电工作站 PyInstaller 打包执行脚本
使用 LH001-91-ECG-Station.spec 规范文件执行单文件 .exe 打包。
自动适配 Windows 11 环境下安全中心/Defender 瞬间加锁的大文件处理。
"""

import os
import sys
import time
import shutil
import PyInstaller.__main__
from PyInstaller.utils.win32 import winutils

_orig_set_exe_build_timestamp = winutils.set_exe_build_timestamp
_orig_update_exe_pe_checksum = winutils.update_exe_pe_checksum


def _wait_and_exec(func, *args, name="操作"):
    for _ in range(25):
        try:
            return func(*args)
        except PermissionError:
            time.sleep(1.0)
    print(f"\n[提示] 文件被系统保护/杀毒扫描锁定，安全略过 {name}（不影响程序正常运行）。")
    return None


def safe_set_exe_build_timestamp(exe_path, timestamp):
    time.sleep(3.0)  # 给予系统 Defender 完成初步扫描的释放缓冲
    return _wait_and_exec(_orig_set_exe_build_timestamp, exe_path, timestamp, name="PE时间戳写入")


def safe_update_exe_pe_checksum(exe_path):
    time.sleep(1.0)
    return _wait_and_exec(_orig_update_exe_pe_checksum, exe_path, name="PE校验和写入")


winutils.set_exe_build_timestamp = safe_set_exe_build_timestamp
winutils.update_exe_pe_checksum = safe_update_exe_pe_checksum


def build():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    dist_dir = os.path.join(base_dir, "dist")
    spec_path = os.path.join(base_dir, "LH001-91-ECG-Station.spec")
    csv_path = os.path.join(base_dir, "ecg_standard_data.csv")

    args = [
        spec_path,
        "--clean",
        "--noconfirm",
    ]

    print("=" * 60)
    print("正在基于 Spec 文件打包 LH001-91 6肢体导联心电工作站 ...")
    print(f"Spec 配置文件: {spec_path}")
    print("=" * 60)

    PyInstaller.__main__.run(args)

    # 在 dist 目录额外提供一份外部 ecg_standard_data.csv 供用户编辑或测试
    if os.path.exists(csv_path):
        target_csv = os.path.join(dist_dir, "ecg_standard_data.csv")
        shutil.copy2(csv_path, target_csv)

    exe_path = os.path.join(dist_dir, "LH001-91-ECG-Station.exe")
    if os.path.exists(exe_path):
        size_mb = os.path.getsize(exe_path) / (1024 * 1024)
        print("=" * 60)
        print("★ 打包成功！★")
        print(f"生成文件路径: {exe_path}")
        print(f"Exe 文件大小: {size_mb:.2f} MB")
        print("=" * 60)
    else:
        print("[警告] 打包未生成预期的 Exe 文件，请检查上方日志。")


if __name__ == "__main__":
    build()
