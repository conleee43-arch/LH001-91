# -*- mode: python ; coding: utf-8 -*-
import os
from PyInstaller.utils.hooks import collect_all

base_dir = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()

datas = [
    (os.path.join(base_dir, 'ecg_standard_data.csv'), '.'),
    (os.path.join(base_dir, 'app_icon.ico'), '.'),
]
binaries = []
hiddenimports = [
    'serial',
    'serial.tools.list_ports',
    'PySide6.QtOpenGL',
    'PySide6.QtOpenGLWidgets',
    'PySide6.QtSvg',
    'PySide6.QtSvgWidgets',
]

# 收集 pyqtgraph 全部依赖
tmp_pyqtgraph = collect_all('pyqtgraph')
datas += tmp_pyqtgraph[0]
binaries += tmp_pyqtgraph[1]
hiddenimports += tmp_pyqtgraph[2]

# 收集 numpy 2.x 全部子模块与私有 C 扩展（避免 numpy._core._exceptions 缺失）
tmp_numpy = collect_all('numpy')
datas += tmp_numpy[0]
binaries += tmp_numpy[1]
hiddenimports += tmp_numpy[2]

# 严格排除其他 Qt 绑定库及无关重量级库，避免与 PySide6 冲突
excludes = [
    'PyQt5',
    'PyQt5.QtCore',
    'PyQt5.QtGui',
    'PyQt5.QtWidgets',
    'PyQt6',
    'PyQt6.QtCore',
    'PyQt6.QtGui',
    'PyQt6.QtWidgets',
    'PySide2',
    'tkinter',
    'matplotlib',
    'IPython',
    'jupyter',
    'notebook',
    'tornado',
    'zmq',
    'scipy',
    'OpenGL',
    'pyqtgraph.opengl',
]

a = Analysis(
    [os.path.join(base_dir, 'main.py')],
    pathex=[base_dir],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=excludes,
    noarchive=False,
    optimize=0,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='LH001-91-ECG-Station',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=os.path.join(base_dir, 'app_icon.ico'),
)
