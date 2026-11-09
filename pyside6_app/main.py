"""
LH001-91 双芯片 ADC 串口上位机 & 6肢体导联心电图工作站 (PySide6)
Target hardware: STM32H750 + Dual LH001-91 (ECG/Bio-potential AFEs)
Features: Real-time 6-limb-lead ECG wave plotting (I, II, III, aVR, aVL, aVF),
          Software ECG Simulator mode, Lead-off status parsing (LOFFSTAT),
          non-blocking multithreaded architecture with binary/ASCII parsing.
"""

import os
import sys
import time
import re
import queue
import traceback
import multiprocessing
from datetime import datetime
import numpy as np
import serial
import serial.tools.list_ports


def get_base_dir() -> str:
    """获取资源文件所在目录（兼容 PyInstaller 单文件打包环境中的临时解压目录）"""
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        return sys._MEIPASS
    return os.path.dirname(os.path.abspath(__file__))


def get_app_dir() -> str:
    """获取程序运行工作目录（打包后为 exe 所在目录，源码运行为 main.py 所在目录）"""
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


# 日志文件存储路径（持久化保存在 exe/脚本 同级目录下）
LOG_FILE_PATH = os.path.join(get_app_dir(), "log.txt")
STARTUP_LOG_PATH = os.path.join(get_app_dir(), "startup.log")

try:
    with open(STARTUP_LOG_PATH, "a", encoding="utf-8") as _f:
        _f.write(f"[{datetime.now()}] Module main.py loading...\n")
except Exception:
    pass


def log_uncaught_exceptions(ex_cls, ex, tb):
    err = "".join(traceback.format_exception(ex_cls, ex, tb))
    print("CRITICAL ERROR:", err, file=sys.stderr)
    try:
        crash_log = os.path.join(get_app_dir(), "crash.log")
        with open(crash_log, "a", encoding="utf-8") as f:
            f.write(f"\n[{datetime.now()}] Uncaught Exception:\n" + err + "\n")
    except Exception:
        pass
    sys.__excepthook__(ex_cls, ex, tb)


sys.excepthook = log_uncaught_exceptions

from PySide6.QtCore import Qt, QThread, Signal, Slot, QTimer
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QGridLayout, QGroupBox, QLabel, QComboBox, QPushButton, QCheckBox,
    QTextEdit, QFrame, QSplitter, QFileDialog
)
from PySide6.QtGui import QFont, QTextCursor, QIcon

os.environ["PYQTGRAPH_QT_LIB"] = "PySide6"
import pyqtgraph as pg

# Configure pyqtgraph for dark aesthetic matching medical standard
pg.setConfigOption('background', '#0b0e14')
pg.setConfigOption('foreground', '#8a99a8')
pg.setConfigOption('antialias', True)

SAMPLE_BYTES = 6
CHIP_COUNT = 2
SLOT_BYTES = SAMPLE_BYTES * CHIP_COUNT           # 12 字节/时间槽 (主机 6B + 从机 6B)
FRAME_MAX_SLOTS = 16

# 线序常量，与固件 Core/Inc/ecg_stream.h 一一对应
SOF0 = 0xA5
SOF1 = 0x5A
FRAME_VERSION = 0x01
FRAME_HEADER_BYTES = 8
FRAME_CRC_BYTES = 2
FRAME_MAX_BYTES = FRAME_HEADER_BYTES + FRAME_MAX_SLOTS * SLOT_BYTES + FRAME_CRC_BYTES

OFF_VERSION = 2
OFF_SLOT_COUNT = 3
OFF_SEQ = 4
OFF_PAYLOAD = FRAME_HEADER_BYTES

CRC16_INIT = 0xFFFF
CRC16_POLY = 0x1021

# PGA Gain = 6, VREF = 2.5V = 2500mV, 24-bit Signed ADC => 2^23 - 1 = 8388607
VREF_MV = 2500.0
PGA_GAIN = 6.0
MAX_ADC_CODE = 8388607.0

# 接收缓冲上限：防止解析停滞或乱码时 rx_buffer 无界增长
MAX_RX_BUFFER = 8192
# 数据队列上限：满时丢弃最旧批次，保证实时性且不让内存无界增长
MAX_QUEUE_ITEMS = 200
# 串口异常日志最小间隔（秒），防止设备掉线后日志刷屏拖死 GUI
ERROR_LOG_MIN_INTERVAL = 1.0
# 普通日志令牌桶：每 LOG_WINDOW 秒最多放行 LOG_BUDGET 条。
LOG_WINDOW = 0.5
LOG_BUDGET = 60
# 连续异常次数阈值：超过则判定设备已断开，主动退出接收线程
MAX_CONSECUTIVE_ERRORS = 20


class ParseMode:
    """解析模式：二进制帧 (SOF+CRC)、ASCII 文本、软件心电模拟器、CSV 标准数据回放。"""
    ASCII = 0
    BINARY = 1
    SIMULATOR = 2
    CSV_REPLAY = 3


def code_to_mv(code: int) -> float:
    """Convert 24-bit signed ADC code to differential voltage (mV)."""
    return (code * VREF_MV) / (MAX_ADC_CODE * PGA_GAIN)


SOF_BYTES = bytes((SOF0, SOF1))


def _build_crc16_table() -> list[int]:
    """CRC-16/CCITT-FALSE 查表，与固件 ecg_crc16() 逐位实现等价。"""
    table = []
    for byte in range(256):
        crc = byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ CRC16_POLY) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
        table.append(crc)
    return table


CRC16_TABLE = _build_crc16_table()


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, 无反转, 无末异或)。"""
    crc = CRC16_INIT
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ CRC16_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


def decode_samples(arr: np.ndarray) -> tuple[np.ndarray, int]:
    """
    向量化解码单芯片的 N 个 6 字节样本，返回 (codes, 该批次按位或融合的 loffstat)。
    """
    codes = (((arr[:, 2].astype(np.int32) & 0x03) << 22)
             | (arr[:, 3].astype(np.int32) << 14)
             | (arr[:, 4].astype(np.int32) << 6)
             | (arr[:, 5].astype(np.int32) >> 2))
    # 24 位有符号数扩展
    codes = np.where(codes & 0x800000, codes - 0x1000000, codes).astype(np.int32)

    # 批次内全部样本按位或融合，防止 50Hz 工频感应造成的单点漏报
    loff_arr = ((arr[:, 0].astype(np.int32) & 0x0F) << 2) | (arr[:, 1].astype(np.int32) >> 6)
    merged_loff = int(np.bitwise_or.reduce(loff_arr))
    return codes, merged_loff


def decode_payload(payload: bytes, slot_count: int) -> tuple[np.ndarray, np.ndarray, int, int]:
    """
    解交叉：载荷为 slot_count 个 12 字节时间槽，每槽主机 6B 紧接从机 6B。
    同一槽内的主从两点采自同一个 1ms 时刻。
    返回 (master_codes, slave_codes, master_loff, slave_loff)。
    """
    arr = np.frombuffer(payload, dtype=np.uint8).reshape(slot_count, SLOT_BYTES)
    m_codes, m_loff = decode_samples(arr[:, :SAMPLE_BYTES])
    s_codes, s_loff = decode_samples(arr[:, SAMPLE_BYTES:])
    return m_codes, s_codes, m_loff, s_loff


def format_loffstat(loff: int, is_slave: bool = False) -> tuple[str, str]:
    """Returns (display_text, color_hex) for Lead-off status with electrode mapping."""
    if loff == 0:
        return "正常 (连接良好)", "#00e676"  # Green

    details = []
    if loff & 0x01: details.append("LL (AIN0+)" if is_slave else "LA (AIN0+)")
    if loff & 0x02: details.append("LL (AIN0-)" if is_slave else "LA (AIN0-)")
    if loff & 0x04: details.append("RA (AIN1+)")
    if loff & 0x08: details.append("RA (AIN1-)")
    if loff & 0x10: details.append("RLD")
    if loff & 0x20: details.append("RLD-")

    desc = ", ".join(details) if details else f"0x{loff:02X}"
    return f"导联脱落 [{desc}]", "#ff3366"  # Red/Pink


# =====================================================================
# LEAD-OFF DEBOUNCER (0.4s Hysteresis Hold-off Filter)
# =====================================================================
class LeadOffDebouncer:
    """
    导联脱落状态消抖器 (带时间滞后保持)：
    - 解决悬空导联线受空间 50Hz 工频/杂散电荷感应导致的脱落位高频跳变。
    - 只要检测到脱落位置 1，立即置位并维持 hold_time 秒（默认 0.4s）。
    - 连续超过 hold_time 未检测到脱落脉冲，才自动恢复为正常连接。
    """
    def __init__(self, hold_time_sec: float = 0.4):
        self.hold_time_sec = hold_time_sec
        self.last_trigger_times = np.zeros(6, dtype=np.float64)

    def update(self, raw_loff: int) -> int:
        now = time.monotonic()
        for bit in range(6):
            if (raw_loff >> bit) & 1:
                self.last_trigger_times[bit] = now

        filtered_loff = 0
        for bit in range(6):
            if (now - self.last_trigger_times[bit]) < self.hold_time_sec:
                filtered_loff |= (1 << bit)

        return filtered_loff

    def reset(self):
        self.last_trigger_times.fill(0.0)


# =====================================================================
# REAL-TIME ECG DSP FILTER (1000Hz)
# =====================================================================
class ECGFilter:
    """
    Real-time 1000Hz ECG DSP Filters (Pure NumPy implementation):
    - 0.5Hz High-Pass (Baseline Wander Removal)
    - 50Hz Notch (Powerline Interference Removal)
    - 35Hz Low-Pass (EMG / High Frequency Noise Removal)
    """
    def __init__(self, fs=1000.0):
        self.fs = fs

        # 1. 0.5Hz High-Pass Butterworth Coefficients (2nd order)
        fc_hp = 0.5
        K_hp = np.tan(np.pi * fc_hp / fs)
        norm_hp = 1.0 + np.sqrt(2) * K_hp + K_hp**2
        b0_hp = 1.0 / norm_hp
        b1_hp = -2.0 / norm_hp
        b2_hp = 1.0 / norm_hp
        a1_hp = 2.0 * (K_hp**2 - 1.0) / norm_hp
        a2_hp = (1.0 - np.sqrt(2) * K_hp + K_hp**2) / norm_hp
        self.b_hp = np.array([b0_hp, b1_hp, b2_hp])
        self.a_hp = np.array([1.0, a1_hp, a2_hp])

        # 2. 50Hz Notch Coefficients (r=0.95)
        f0_notch, r_notch = 50.0, 0.95
        w0 = 2.0 * np.pi * f0_notch / fs
        b0_n, b1_n, b2_n = 1.0, -2.0 * np.cos(w0), 1.0
        a0_n, a1_n, a2_n = 1.0, -2.0 * r_notch * np.cos(w0), r_notch * r_notch
        g_n = (1.0 + a1_n + a2_n) / (1.0 + b1_n + b2_n)
        self.b_notch = np.array([b0_n, b1_n, b2_n]) * g_n
        self.a_notch = np.array([a0_n, a1_n, a2_n])

        # 3. 35Hz Low-Pass Butterworth Coefficients (2nd order)
        fc_lp = 35.0
        K_lp = np.tan(np.pi * fc_lp / fs)
        norm_lp = 1.0 + np.sqrt(2) * K_lp + K_lp**2
        b0_lp = K_lp**2 / norm_lp
        b1_lp = 2.0 * b0_lp
        b2_lp = b0_lp
        a1_lp = 2.0 * (K_lp**2 - 1.0) / norm_lp
        a2_lp = (1.0 - np.sqrt(2) * K_lp + K_lp**2) / norm_lp
        self.b_lp = np.array([b0_lp, b1_lp, b2_lp])
        self.a_lp = np.array([1.0, a1_lp, a2_lp])

    @staticmethod
    def _apply_iir(b: np.ndarray, a: np.ndarray, x: np.ndarray) -> np.ndarray:
        y = np.zeros_like(x, dtype=np.float64)
        b0, b1, b2 = b[0], b[1], b[2]
        a1, a2 = a[1], a[2]
        w1 = w2 = 0.0
        for i in range(len(x)):
            xi = float(x[i])
            yi = b0 * xi + w1
            w1 = b1 * xi - a1 * yi + w2
            w2 = b2 * xi - a2 * yi
            y[i] = yi
        return y

    def process(self, signal: np.ndarray, hp: bool = True, notch: bool = True, lp: bool = True) -> np.ndarray:
        out = signal.copy()
        if hp:
            out = self._apply_iir(self.b_hp, self.a_hp, out)
        if notch:
            out = self._apply_iir(self.b_notch, self.a_notch, out)
        if lp:
            out = self._apply_iir(self.b_lp, self.a_lp, out)
        return out


# =====================================================================
# ECG SIGNAL SIMULATOR
# =====================================================================
class ECGSimulator:
    """
    Generates synthetic Lead I and Lead II ECG waves at 1000Hz sampling rate,
    modulating respiration baseline wander, powerline interference (50Hz), and QRS.
    """
    def __init__(self, fs=1000):
        self.fs = fs
        self.t = 0
        self.heart_rate = 75  # Beats per minute
        self.period = 60.0 / self.heart_rate  # Time per beat in seconds
        self.lsd_factor = VREF_MV / (PGA_GAIN * MAX_ADC_CODE)

    def generate_sample(self) -> tuple[int, int]:
        self.t += 1.0 / self.fs
        beat_t = self.t % self.period

        # QRS Wave Simulation (using localized Gaussians)
        qrs_i = 1.2 * np.exp(-((beat_t - 0.2) / 0.015)**2)
        qrs_ii = 1.6 * np.exp(-((beat_t - 0.2) / 0.015)**2)

        # P Wave
        p_i = 0.15 * np.exp(-((beat_t - 0.1) / 0.030)**2)
        p_ii = 0.20 * np.exp(-((beat_t - 0.1) / 0.030)**2)

        # T Wave
        t_i = 0.25 * np.exp(-((beat_t - 0.45) / 0.050)**2)
        t_ii = 0.35 * np.exp(-((beat_t - 0.45) / 0.050)**2)

        # Baseline wander & 50Hz noise
        wander_i = 0.15 * np.sin(2 * np.pi * 0.2 * self.t)
        wander_ii = 0.12 * np.sin(2 * np.pi * 0.23 * self.t)
        noise_50hz = 0.05 * np.sin(2 * np.pi * 50 * self.t)

        lead_i = p_i + qrs_i + t_i + wander_i + noise_50hz
        lead_ii = p_ii + qrs_ii + t_ii + wander_ii + noise_50hz

        code_i = int(lead_i / self.lsd_factor)
        code_ii = int(lead_ii / self.lsd_factor)

        code_i = max(-8388608, min(8388607, code_i))
        code_ii = max(-8388608, min(8388607, code_ii))

        return code_i, code_ii


# =====================================================================
# SERIAL RECEIVER & SIMULATOR THREAD
# =====================================================================
class SerialReceiverThread(QThread):
    log_received = Signal(str)
    connection_status = Signal(bool, str)

    def __init__(self, port: str, baudrate: int, data_queue: queue.Queue,
                 parse_mode: int = ParseMode.BINARY, csv_path: str = None):
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self.data_queue = data_queue
        self.parse_mode = parse_mode
        self.csv_path = csv_path
        self.is_running = False
        self.serial_port = None

        # 解析状态
        self.rx_buffer = bytearray()
        self.current_m_loff = 0
        self.current_s_loff = 0

        # 链路质量统计
        self.frame_count = 0
        self.crc_error_count = 0
        self.seq_gap_count = 0
        self.last_seq = None

        # 日志限流状态
        self._last_data_log = 0.0
        self._last_error_log = 0.0
        self._log_window_start = 0.0
        self._log_window_count = 0
        self._log_suppressed = 0

        self.ascii_pattern = re.compile(r"\[ADC CODE\]\s*Master:\s*(-?\d+)\s*\|\s*Slave:\s*(-?\d+)")
        self.ascii_mv_pattern = re.compile(r"\[ADC mV\]\s*Master:\s*([-+]?\d*\.?\d+)\s*mV\s*\|\s*Slave:\s*([-+]?\d*\.?\d+)\s*mV")
        self.loff_pattern = re.compile(r"LOFFSTAT:\s*M=0x([0-9A-Fa-f]+),\s*S=0x([0-9A-Fa-f]+)")

    def stop(self):
        """请求线程退出。"""
        self.is_running = False
        if not self.wait(3000):
            self.terminate()
            self.wait(1000)

    def _put_data(self, m_codes, s_codes, m_loff, s_loff):
        """入队，队列满时丢弃最旧批次以保证实时性。"""
        if self.data_queue.qsize() >= MAX_QUEUE_ITEMS:
            try:
                self.data_queue.get_nowait()
            except queue.Empty:
                pass
        self.data_queue.put((m_codes, s_codes, m_loff, s_loff))

    def _log_rate_limited(self, text: str):
        self._flush_log_suppression()
        if self._log_window_count < LOG_BUDGET:
            self.log_received.emit(text)
            self._log_window_count += 1
        else:
            self._log_suppressed += 1

    def _flush_log_suppression(self):
        now = time.monotonic()
        if now - self._log_window_start < LOG_WINDOW:
            return
        if self._log_suppressed > 0:
            self.log_received.emit(f"[提示] 输出过快，已抑制 {self._log_suppressed} 条日志")
            self._log_suppressed = 0
        self._log_window_start = now
        self._log_window_count = 0

    def run(self):
        if self.parse_mode == ParseMode.SIMULATOR:
            self._run_simulator()
            return
        elif self.parse_mode == ParseMode.CSV_REPLAY:
            self._run_csv_replay()
            return

        try:
            self.serial_port = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=0.1,
                rtscts=False,
                dsrdtr=False
            )
            self.connection_status.emit(True, f"已连接 {self.port} @ {self.baudrate}")
        except Exception as e:
            self.connection_status.emit(False, f"无法打开串口: {e}")
            return

        self.is_running = True
        consecutive_errors = 0

        while self.is_running:
            try:
                waiting_bytes = self.serial_port.in_waiting
                if waiting_bytes > 0:
                    self.rx_buffer.extend(self.serial_port.read(waiting_bytes))
                    consecutive_errors = 0
                else:
                    self._flush_log_suppression()
                    time.sleep(0.001)
                    continue

                if len(self.rx_buffer) > MAX_RX_BUFFER:
                    dropped = len(self.rx_buffer) - MAX_RX_BUFFER
                    del self.rx_buffer[:dropped]
                    self._log_rate_limited(f"[警告] 接收缓冲溢出，丢弃 {dropped} 字节")

                if self.parse_mode == ParseMode.ASCII:
                    self._parse_ascii()
                else:
                    self._parse_binary()

            except Exception as e:
                consecutive_errors += 1
                now = time.monotonic()
                if self.is_running and (now - self._last_error_log >= ERROR_LOG_MIN_INTERVAL):
                    self.log_received.emit(f"[错误] 串口通信异常: {e}")
                    self._last_error_log = now
                if consecutive_errors >= MAX_CONSECUTIVE_ERRORS:
                    if self.is_running:
                        self.log_received.emit("[错误] 串口连续异常，判定设备已断开")
                    break
                time.sleep(0.05)

        try:
            if self.serial_port and self.serial_port.is_open:
                self.serial_port.close()
        except Exception:
            pass
        self.connection_status.emit(False, "串口已关闭")

    def _run_simulator(self):
        """运行软件心电图模拟器，平滑推送 1000Hz 合成 6 导联数据。"""
        self.is_running = True
        self.connection_status.emit(True, "已启动软件心电图模拟器 (1000Hz)")
        self.log_received.emit("[系统] 软件模拟模式：正在合成标准 6 肢体导联 ECG 信号...")

        simulator = ECGSimulator(fs=1000)
        last_log_time = time.monotonic()
        next_sample_time = time.monotonic()
        seq = 0

        while self.is_running:
            now = time.monotonic()
            m_list = []
            s_list = []

            while now >= next_sample_time and len(m_list) < 20 and self.is_running:
                cm, cs = simulator.generate_sample()
                m_list.append(cm)
                s_list.append(cs)
                next_sample_time += 0.001
                seq += 1

            if m_list:
                m_arr = np.array(m_list, dtype=np.int32)
                s_arr = np.array(s_list, dtype=np.int32)
                self._put_data(m_arr, s_arr, 0, 0)

                if now - last_log_time >= 0.5:
                    self.log_received.emit(
                        f"[SIMULATOR] seq={seq} | Lead I(Master)={m_arr[-1]} | Lead II(Slave)={s_arr[-1]}"
                    )
                    last_log_time = now

            time.sleep(0.005)

        self.connection_status.emit(False, "模拟器已停止")

    def _run_csv_replay(self):
        """Streams ECG samples directly from ecg_standard_data.csv at 1000Hz."""
        csv_file_path = self.csv_path
        if not csv_file_path or not os.path.exists(csv_file_path):
            cand_local = os.path.join(get_app_dir(), "ecg_standard_data.csv")
            cand_bundle = os.path.join(get_base_dir(), "ecg_standard_data.csv")
            if os.path.exists(cand_local):
                csv_file_path = cand_local
            elif os.path.exists(cand_bundle):
                csv_file_path = cand_bundle
            else:
                csv_file_path = cand_local

        if not os.path.exists(csv_file_path):
            self.connection_status.emit(False, f"CSV 文件不存在: {csv_file_path}")
            return

        self.is_running = True
        self.connection_status.emit(True, f"已载入 CSV 数据集 ({os.path.basename(csv_file_path)})")
        self.log_received.emit(f"[系统] CSV 数据推流模式：读取 {os.path.basename(csv_file_path)} 并按 1000Hz 实时推流...")

        try:
            import pandas as pd
            df = pd.read_csv(csv_file_path)
            if 'Noisy_Lead_I_mV' in df.columns and 'Noisy_Lead_II_mV' in df.columns:
                i_vals = df['Noisy_Lead_I_mV'].values
                ii_vals = df['Noisy_Lead_II_mV'].values
            elif 'Clean_Lead_I_mV' in df.columns and 'Clean_Lead_II_mV' in df.columns:
                i_vals = df['Clean_Lead_I_mV'].values
                ii_vals = df['Clean_Lead_II_mV'].values
            else:
                numeric_cols = df.select_dtypes(include=[np.number]).columns
                if len(numeric_cols) >= 3:
                    i_vals = df[numeric_cols[1]].values
                    ii_vals = df[numeric_cols[2]].values
                else:
                    self.connection_status.emit(False, "CSV 文件列不兼容")
                    return
        except Exception as e:
            self.connection_status.emit(False, f"读取 CSV 失败: {e}")
            return

        total_samples = len(i_vals)
        lsd_factor = VREF_MV / (PGA_GAIN * MAX_ADC_CODE)
        idx = 0
        seq = 0
        last_log_time = time.monotonic()
        next_sample_time = time.monotonic()

        while self.is_running:
            now = time.monotonic()
            m_list = []
            s_list = []

            while now >= next_sample_time and len(m_list) < 20 and self.is_running:
                mv1 = float(i_vals[idx % total_samples])
                mv2 = float(ii_vals[idx % total_samples])

                code_i = int(mv1 / lsd_factor)
                code_ii = int(mv2 / lsd_factor)

                m_list.append(code_i)
                s_list.append(code_ii)

                idx += 1
                seq += 1
                next_sample_time += 0.001

            if m_list:
                m_arr = np.array(m_list, dtype=np.int32)
                s_arr = np.array(s_list, dtype=np.int32)
                self._put_data(m_arr, s_arr, 0, 0)

                if now - last_log_time >= 0.5:
                    cur_idx = (idx - 1) % total_samples
                    self.log_received.emit(
                        f"[CSV REPLAY] sample {cur_idx + 1}/{total_samples} | "
                        f"Lead I={i_vals[cur_idx]:.3f}mV | "
                        f"Lead II={ii_vals[cur_idx]:.3f}mV"
                    )
                    last_log_time = now

            time.sleep(0.005)

        self.connection_status.emit(False, "CSV 数据回放已停止")

    def _parse_ascii(self):
        while True:
            idx = self.rx_buffer.find(b'\n')
            if idx < 0:
                break

            line_bytes = bytes(self.rx_buffer[:idx])
            del self.rx_buffer[:idx + 1]

            line_str = line_bytes.decode('utf-8', errors='replace').strip()
            if not line_str:
                continue

            self._handle_ascii_line(line_str)

    def _handle_ascii_line(self, line_str: str):
        loff_match = self.loff_pattern.search(line_str)
        if loff_match:
            self.current_m_loff = int(loff_match.group(1), 16)
            self.current_s_loff = int(loff_match.group(2), 16)
            self._log_rate_limited(line_str)
            return

        match = self.ascii_pattern.search(line_str)
        if match:
            m_val = int(match.group(1))
            s_val = int(match.group(2))
            self._put_data(
                np.array([m_val], dtype=np.int32),
                np.array([s_val], dtype=np.int32),
                self.current_m_loff,
                self.current_s_loff
            )
            now = time.monotonic()
            if now - self._last_data_log >= 0.15:
                self.log_received.emit(line_str)
                self._last_data_log = now
            return

        mv_match = self.ascii_mv_pattern.search(line_str)
        if mv_match:
            m_mv = float(mv_match.group(1))
            s_mv = float(mv_match.group(2))
            m_val = int((m_mv * PGA_GAIN * MAX_ADC_CODE) / VREF_MV)
            s_val = int((s_mv * PGA_GAIN * MAX_ADC_CODE) / VREF_MV)
            self._put_data(
                np.array([m_val], dtype=np.int32),
                np.array([s_val], dtype=np.int32),
                self.current_m_loff,
                self.current_s_loff
            )
            now = time.monotonic()
            if now - self._last_data_log >= 0.15:
                self.log_received.emit(line_str)
                self._last_data_log = now
            return

        self._log_rate_limited(line_str)

    def _parse_binary(self):
        while True:
            idx = self.rx_buffer.find(SOF_BYTES)
            if idx < 0:
                if len(self.rx_buffer) > 1:
                    self._log_discarded_text(bytes(self.rx_buffer[:-1]))
                    del self.rx_buffer[:-1]
                return

            if idx > 0:
                self._log_discarded_text(bytes(self.rx_buffer[:idx]))
                del self.rx_buffer[:idx]

            if len(self.rx_buffer) < FRAME_HEADER_BYTES:
                return

            if not self._header_is_valid():
                del self.rx_buffer[:2]
                continue

            slot_count = self.rx_buffer[OFF_SLOT_COUNT]
            span = FRAME_HEADER_BYTES + slot_count * SLOT_BYTES
            frame_len = span + FRAME_CRC_BYTES
            if len(self.rx_buffer) < frame_len:
                return

            frame = bytes(self.rx_buffer[:frame_len])
            expect = frame[span] | (frame[span + 1] << 8)
            if expect != crc16(frame[OFF_VERSION:span]):
                self.crc_error_count += 1
                del self.rx_buffer[:2]
                continue

            del self.rx_buffer[:frame_len]
            self._handle_frame(frame, slot_count, span)

    def _header_is_valid(self) -> bool:
        if self.rx_buffer[OFF_VERSION] != FRAME_VERSION:
            return False
        return 1 <= self.rx_buffer[OFF_SLOT_COUNT] <= FRAME_MAX_SLOTS

    def _handle_frame(self, frame: bytes, slot_count: int, span: int):
        seq = int.from_bytes(frame[OFF_SEQ:OFF_SEQ + 4], 'little')
        if self.last_seq is not None:
            gap = (seq - self.last_seq - 1) & 0xFFFFFFFF
            if gap:
                self.seq_gap_count += gap
                self._log_rate_limited(f"[警告] 丢帧 {gap} 帧 (seq {self.last_seq} -> {seq})")
        self.last_seq = seq
        self.frame_count += 1

        m_codes, s_codes, m_loff, s_loff = decode_payload(
            frame[OFF_PAYLOAD:span], slot_count
        )
        self.current_m_loff = m_loff
        self.current_s_loff = s_loff
        self._put_data(m_codes, s_codes, m_loff, s_loff)

        now = time.monotonic()
        if now - self._last_data_log >= 0.2:
            self.log_received.emit(
                f"[ADC CODE] seq={seq} slots={slot_count} | "
                f"Master: {m_codes[0]} (LOFF:0x{m_loff:02X}) | "
                f"Slave: {s_codes[0]} (LOFF:0x{s_loff:02X})"
            )
            self._last_data_log = now

    def _log_discarded_text(self, chunk: bytes):
        if not chunk:
            return
        for line in chunk.decode('utf-8', errors='ignore').splitlines():
            line = line.strip()
            if len(line) >= 3 and line.isprintable():
                self._log_rate_limited(line)


# =====================================================================
# MAIN WINDOW WITH 6-LIMB-LEAD ECG DISPLAY
# =====================================================================
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()

        self.setWindowTitle("LH001-91 双芯片 6 肢体导联心电上位机工作站 (PySide6)")
        self.resize(1280, 850)

        icon_path = os.path.join(get_base_dir(), "app_icon.ico")
        if os.path.exists(icon_path):
            self.setWindowIcon(QIcon(icon_path))

        # Plot parameters: 4000 points ring buffer = 4 seconds window at 1000Hz
        self.buffer_size = 4000
        self.master_code_buf = np.zeros(self.buffer_size, dtype=np.float64)
        self.slave_code_buf = np.zeros(self.buffer_size, dtype=np.float64)
        self.custom_csv_path = None
        # Real-time DSP Filter (1000Hz)
        self.ecg_filter = ECGFilter(fs=1000.0)

        # 导联脱落消抖器 (0.4秒滞后保持)
        self.m_debouncer = LeadOffDebouncer(hold_time_sec=0.4)
        self.s_debouncer = LeadOffDebouncer(hold_time_sec=0.4)

        # Thread-safe queue for decoupling receiver thread from GUI render loop
        self.data_queue = queue.Queue()
        self.receiver_thread = None

        self.init_ui()
        self.refresh_ports()

        # UI Update Timer (30 FPS = ~33ms)
        self.ui_timer = QTimer(self)
        self.ui_timer.setInterval(33)
        self.ui_timer.timeout.connect(self.process_queued_data)
        self.ui_timer.start()

    def init_ui(self):
        self.setStyleSheet("""
            QMainWindow {
                background-color: #0d1017;
            }
            QWidget {
                background-color: #0d1017;
                color: #c9d1d9;
                font-family: "Microsoft YaHei", "Segoe UI", sans-serif;
            }
            QGroupBox {
                font-weight: bold;
                font-size: 13px;
                border: 1px solid #21262d;
                border-radius: 6px;
                margin-top: 8px;
                padding-top: 10px;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 12px;
                padding: 0 4px;
                color: #00d2ff;
            }
            QLabel {
                font-size: 12px;
                color: #8b949e;
            }
            QComboBox {
                background-color: #161b22;
                color: #c9d1d9;
                border: 1px solid #30363d;
                border-radius: 4px;
                padding: 3px 8px;
                min-height: 22px;
            }
            QComboBox::drop-down {
                border: none;
            }
            QPushButton {
                background-color: #21262d;
                color: #c9d1d9;
                border: 1px solid #30363d;
                border-radius: 4px;
                padding: 4px 12px;
                font-size: 12px;
                min-height: 22px;
            }
            QPushButton:hover {
                background-color: #30363d;
                border-color: #8b949e;
            }
            QPushButton#open_btn {
                background-color: #238636;
                color: white;
                border: none;
                font-weight: bold;
            }
            QPushButton#open_btn:hover {
                background-color: #2ea043;
            }
            QPushButton#close_btn {
                background-color: #e63946;
                color: white;
                border: none;
                font-weight: bold;
            }
            QPushButton#close_btn:hover {
                background-color: #ff4d6d;
            }
            QCheckBox {
                color: #8b949e;
                font-size: 12px;
            }
            QCheckBox::indicator {
                width: 14px;
                height: 14px;
            }
        """)

        central_widget = QWidget()
        self.setCentralWidget(central_widget)

        main_layout = QVBoxLayout(central_widget)
        main_layout.setContentsMargins(10, 8, 10, 10)
        main_layout.setSpacing(8)

        # -------------------------------------------------------------
        # 1. Top Control Bar (串口连接设置 & 波形解析模式)
        # -------------------------------------------------------------
        top_bar_layout = QHBoxLayout()

        conn_group = QGroupBox("串口连接与模式控制")
        conn_layout = QHBoxLayout(conn_group)
        conn_layout.setContentsMargins(10, 8, 10, 8)
        conn_layout.setSpacing(10)

        conn_layout.addWidget(QLabel("串口号:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(200)
        conn_layout.addWidget(self.port_combo)

        self.refresh_btn = QPushButton("刷新串口")
        self.refresh_btn.clicked.connect(self.refresh_ports)
        conn_layout.addWidget(self.refresh_btn)

        conn_layout.addWidget(QLabel("波特率:"))
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "460800", "921600", "1500000", "2000000", "3000000"])
        self.baud_combo.setCurrentText("1500000")
        conn_layout.addWidget(self.baud_combo)

        conn_layout.addWidget(QLabel("工作模式:"))
        self.parse_mode_combo = QComboBox()
        self.parse_mode_combo.addItems(["ASCII 文本", "二进制帧 (SOF+CRC)", "软件模拟器 (Simulator)", "CSV 参考数据回放 (CSV Data)"])
        self.parse_mode_combo.setCurrentIndex(ParseMode.BINARY)
        self.parse_mode_combo.currentIndexChanged.connect(self.on_parse_mode_changed)
        conn_layout.addWidget(self.parse_mode_combo)

        self.btn_select_csv = QPushButton("选择 CSV")
        self.btn_select_csv.setToolTip("选择自定义 CSV 数据文件（默认使用 ecg_standard_data.csv）")
        self.btn_select_csv.setVisible(False)
        self.btn_select_csv.clicked.connect(self.select_csv_file)
        conn_layout.addWidget(self.btn_select_csv)

        self.toggle_conn_btn = QPushButton("打开采集")
        self.toggle_conn_btn.setObjectName("open_btn")
        self.toggle_conn_btn.setMinimumWidth(90)
        self.toggle_conn_btn.clicked.connect(self.toggle_connection)
        conn_layout.addWidget(self.toggle_conn_btn)

        top_bar_layout.addWidget(conn_group)

        filter_group = QGroupBox("实时数字滤波 (1000Hz DSP)")
        filter_layout = QHBoxLayout(filter_group)
        filter_layout.setContentsMargins(10, 8, 10, 8)
        filter_layout.setSpacing(10)

        self.filter_enable_check = QCheckBox("启用滤波")
        self.filter_enable_check.setChecked(True)
        self.filter_enable_check.stateChanged.connect(self.on_filter_changed)

        self.hp_check = QCheckBox("0.5Hz高通")
        self.hp_check.setChecked(True)
        self.hp_check.setToolTip("消除呼吸引起的基线漂移 (Baseline Wander)")
        self.hp_check.stateChanged.connect(self.on_filter_changed)

        self.notch_check = QCheckBox("50Hz陷波")
        self.notch_check.setChecked(True)
        self.notch_check.setToolTip("消除市电 50Hz 工频干扰")
        self.notch_check.stateChanged.connect(self.on_filter_changed)

        self.lp_check = QCheckBox("35Hz低通")
        self.lp_check.setChecked(True)
        self.lp_check.setToolTip("消除肌电干扰 (EMG Noise) 及高频毛刺")
        self.lp_check.stateChanged.connect(self.on_filter_changed)

        filter_layout.addWidget(self.filter_enable_check)
        filter_layout.addWidget(self.hp_check)
        filter_layout.addWidget(self.notch_check)
        filter_layout.addWidget(self.lp_check)

        top_bar_layout.addWidget(filter_group)

        top_bar_layout.addStretch()

        right_mode_layout = QHBoxLayout()
        self.autoscale_check = QCheckBox("Y轴幅值自适应缩放")
        self.autoscale_check.setChecked(True)
        self.autoscale_check.stateChanged.connect(self.on_autoscale_changed)
        right_mode_layout.addWidget(self.autoscale_check)

        right_mode_layout.addSpacing(10)
        right_mode_layout.addWidget(QLabel("波形绘制刻度:"))
        self.plot_value_mode_combo = QComboBox()
        self.plot_value_mode_combo.addItems(["电压值 (mV)", "ADC Code"])
        self.plot_value_mode_combo.currentIndexChanged.connect(self.on_plot_mode_changed)
        right_mode_layout.addWidget(self.plot_value_mode_combo)

        top_bar_layout.addLayout(right_mode_layout)

        main_layout.addLayout(top_bar_layout)

        # -------------------------------------------------------------
        # 2. Middle Section: Numeric Readouts & 6-Limb-Lead Grid
        # -------------------------------------------------------------
        middle_layout = QHBoxLayout()
        middle_layout.setSpacing(10)

        # ---- Left Panel: Real-time Status Readout ----
        status_group = QGroupBox("芯片状态与数值")
        status_layout = QVBoxLayout(status_group)
        status_layout.setContentsMargins(10, 10, 10, 10)
        status_layout.setSpacing(8)

        info_grid = QGridLayout()
        info_grid.setVerticalSpacing(6)

        # Master Info (Channel 1 / Lead I)
        info_grid.addWidget(QLabel("<b>[Master - 通道1]</b>"), 0, 0, 1, 2)
        info_grid.addWidget(QLabel("最新 Code:"), 1, 0)
        self.master_code_val = QLabel("--")
        self.master_code_val.setStyleSheet("color: #00e5ff; font-size: 14px; font-weight: bold;")
        info_grid.addWidget(self.master_code_val, 1, 1, Qt.AlignRight)

        info_grid.addWidget(QLabel("电压 (mV):"), 2, 0)
        self.master_mv_val = QLabel("-- mV")
        self.master_mv_val.setStyleSheet("color: #ffea00; font-size: 14px; font-weight: bold;")
        info_grid.addWidget(self.master_mv_val, 2, 1, Qt.AlignRight)

        info_grid.addWidget(QLabel("脱落状态:"), 3, 0)
        self.master_loff_val = QLabel("未知")
        self.master_loff_val.setStyleSheet("color: #8b949e; font-size: 12px; font-weight: bold;")
        info_grid.addWidget(self.master_loff_val, 3, 1, Qt.AlignRight)

        # Separator line
        sep = QFrame()
        sep.setFrameShape(QFrame.HLine)
        sep.setStyleSheet("color: #30363d;")
        info_grid.addWidget(sep, 4, 0, 1, 2)

        # Slave Info (Channel 2 / Lead II)
        info_grid.addWidget(QLabel("<b>[Slave - 通道2]</b>"), 5, 0, 1, 2)
        info_grid.addWidget(QLabel("最新 Code:"), 6, 0)
        self.slave_code_val = QLabel("--")
        self.slave_code_val.setStyleSheet("color: #ff007f; font-size: 14px; font-weight: bold;")
        info_grid.addWidget(self.slave_code_val, 6, 1, Qt.AlignRight)

        info_grid.addWidget(QLabel("电压 (mV):"), 7, 0)
        self.slave_mv_val = QLabel("-- mV")
        self.slave_mv_val.setStyleSheet("color: #ffea00; font-size: 14px; font-weight: bold;")
        info_grid.addWidget(self.slave_mv_val, 7, 1, Qt.AlignRight)

        info_grid.addWidget(QLabel("脱落状态:"), 8, 0)
        self.slave_loff_val = QLabel("未知")
        self.slave_loff_val.setStyleSheet("color: #8b949e; font-size: 12px; font-weight: bold;")
        info_grid.addWidget(self.slave_loff_val, 8, 1, Qt.AlignRight)

        status_layout.addLayout(info_grid)
        status_layout.addStretch()

        # Math formula guidance
        formula_lbl = QLabel(
            "<b>肢体导联推导公式:</b><br>"
            "Lead I = Master<br>"
            "Lead II = Slave<br>"
            "Lead III = II - I<br>"
            "aVR = -(I + II) / 2<br>"
            "aVL = I - II / 2<br>"
            "aVF = II - I / 2"
        )
        formula_lbl.setStyleSheet("color: #8b949e; font-size: 11px; line-height: 1.4;")
        status_layout.addWidget(formula_lbl)

        middle_layout.addWidget(status_group, stretch=1)

        # ---- Right Panel: 3x2 Grid 6-Limb-Lead ECG Plots ----
        plots_group = QGroupBox("标准 6 肢体导联心电图实时波形 (6-Limb-Lead Workstation)")
        plots_grid_layout = QGridLayout(plots_group)
        plots_grid_layout.setContentsMargins(8, 8, 8, 8)
        plots_grid_layout.setSpacing(6)

        self.plot_widgets = {}
        self.curve_items = {}

        # 3x2 Grid positions: (row, col)
        lead_positions = {
            'I': (0, 0),   'aVR': (0, 1),
            'II': (1, 0),  'aVL': (1, 1),
            'III': (2, 0), 'aVF': (2, 1)
        }

        lead_colors = {
            'I': '#00e5ff',   'aVR': '#ff007f',
            'II': '#00ff88',  'aVL': '#ffea00',
            'III': '#ff9100', 'aVF': '#d500f9'
        }

        first_plot = None

        for name, (row, col) in lead_positions.items():
            plot = pg.PlotWidget(title=f"<b>Lead {name}</b>")
            plot.showGrid(x=True, y=True, alpha=0.2)
            plot.enableAutoRange(axis='y')
            plot.setXRange(0, self.buffer_size)
            plot.getAxis('bottom').setLabel("Samples (4s window @ 1000Hz)")
            plot.getAxis('left').setLabel("mV")

            pen = pg.mkPen(color=lead_colors[name], width=1.5)
            curve = plot.plot(pen=pen)

            if first_plot is None:
                first_plot = plot
            else:
                plot.setXLink(first_plot)

            plots_grid_layout.addWidget(plot, row, col)
            self.plot_widgets[name] = plot
            self.curve_items[name] = curve

        middle_layout.addWidget(plots_group, stretch=4)

        main_layout.addLayout(middle_layout, stretch=4)

        # -------------------------------------------------------------
        # 3. Bottom Section (串口 RAW 信息日志)
        # -------------------------------------------------------------
        log_group = QGroupBox("串口 RAW 信息日志")
        log_layout = QVBoxLayout(log_group)
        log_layout.setContentsMargins(10, 8, 10, 8)
        log_layout.setSpacing(6)

        log_ctrl_layout = QHBoxLayout()
        self.autoscroll_check = QCheckBox("自动滚动")
        self.autoscroll_check.setChecked(True)
        log_ctrl_layout.addWidget(self.autoscroll_check)

        clear_log_btn = QPushButton("清空日志")
        clear_log_btn.clicked.connect(self.clear_log)
        log_ctrl_layout.addWidget(clear_log_btn)

        log_ctrl_layout.addStretch()
        log_layout.addLayout(log_ctrl_layout)

        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumHeight(130)
        self.log_text.document().setMaximumBlockCount(300)

        self.log_text.setStyleSheet("""
            QTextEdit {
                background-color: #080a0f;
                color: #00ff66;
                font-family: Consolas, "Courier New", monospace;
                font-size: 12px;
                border: 1px solid #1f242e;
                border-radius: 4px;
            }
        """)
        log_layout.addWidget(self.log_text)

        main_layout.addWidget(log_group, stretch=1)

    def refresh_ports(self):
        self.port_combo.clear()
        ports = serial.tools.list_ports.comports()
        for p in ports:
            self.port_combo.addItem(f"{p.device} ({p.description})", p.device)
        if not ports:
            self.port_combo.addItem("无可用串口", "")

    def select_csv_file(self):
        file_path, _ = QFileDialog.getOpenFileName(
            self, "选择 CSV 参考心电数据文件", "", "CSV Files (*.csv);;All Files (*)"
        )
        if file_path:
            self.custom_csv_path = file_path
            self.append_log(f"[系统] 已选择自定义 CSV 数据文件: {file_path}")

    def on_parse_mode_changed(self, index: int):
        is_hardware = (index in (ParseMode.ASCII, ParseMode.BINARY))
        is_csv = (index == ParseMode.CSV_REPLAY)
        self.port_combo.setEnabled(is_hardware)
        self.refresh_btn.setEnabled(is_hardware)
        self.baud_combo.setEnabled(is_hardware)
        self.btn_select_csv.setVisible(is_csv)

    def toggle_connection(self):
        if self.receiver_thread and self.receiver_thread.isRunning():
            self.receiver_thread.stop()
            self.receiver_thread = None

            self.m_debouncer.reset()
            self.s_debouncer.reset()

            self.toggle_conn_btn.setText("打开采集")
            self.toggle_conn_btn.setObjectName("open_btn")
            self.toggle_conn_btn.setStyle(self.toggle_conn_btn.style())
            parse_mode = self.parse_mode_combo.currentIndex()
            is_hardware = (parse_mode in (ParseMode.ASCII, ParseMode.BINARY))
            is_csv = (parse_mode == ParseMode.CSV_REPLAY)
            self.port_combo.setEnabled(is_hardware)
            self.refresh_btn.setEnabled(is_hardware)
            self.baud_combo.setEnabled(is_hardware)
            self.btn_select_csv.setEnabled(True)
            self.btn_select_csv.setVisible(is_csv)
            self.parse_mode_combo.setEnabled(True)
        else:
            parse_mode = self.parse_mode_combo.currentIndex()
            if parse_mode == ParseMode.SIMULATOR:
                port = "SIMULATOR"
                baud = 115200
            elif parse_mode == ParseMode.CSV_REPLAY:
                port = "CSV_REPLAY"
                baud = 115200
            else:
                port = self.port_combo.currentData()
                if not port:
                    return
                baud = int(self.baud_combo.currentText())

            while not self.data_queue.empty():
                try:
                    self.data_queue.get_nowait()
                except queue.Empty:
                    break

            self.m_debouncer.reset()
            self.s_debouncer.reset()

            self.receiver_thread = SerialReceiverThread(
                port, baud, self.data_queue, parse_mode, csv_path=self.custom_csv_path
            )
            self.receiver_thread.log_received.connect(self.append_log)
            self.receiver_thread.connection_status.connect(self.on_connection_status)
            self.receiver_thread.start()

            self.toggle_conn_btn.setText("关闭采集")
            self.toggle_conn_btn.setObjectName("close_btn")
            self.toggle_conn_btn.setStyle(self.toggle_conn_btn.style())
            self.port_combo.setEnabled(False)
            self.refresh_btn.setEnabled(False)
            self.baud_combo.setEnabled(False)
            self.btn_select_csv.setEnabled(False)
            self.parse_mode_combo.setEnabled(False)

    @Slot(bool, str)
    def on_connection_status(self, connected: bool, message: str):
        self.append_log(f"[系统] {message}")
        if not connected and self.toggle_conn_btn.text() == "关闭采集":
            self.toggle_conn_btn.setText("打开采集")
            self.toggle_conn_btn.setObjectName("open_btn")
            self.toggle_conn_btn.setStyle(self.toggle_conn_btn.style())
            parse_mode = self.parse_mode_combo.currentIndex()
            is_hardware = (parse_mode in (ParseMode.ASCII, ParseMode.BINARY))
            is_csv = (parse_mode == ParseMode.CSV_REPLAY)
            self.port_combo.setEnabled(is_hardware)
            self.refresh_btn.setEnabled(is_hardware)
            self.baud_combo.setEnabled(is_hardware)
            self.btn_select_csv.setEnabled(True)
            self.btn_select_csv.setVisible(is_csv)
            self.parse_mode_combo.setEnabled(True)
            self.m_debouncer.reset()
            self.s_debouncer.reset()

    def process_queued_data(self):
        """Timer callback (30 FPS) to consume data queue and update GUI smoothly."""
        master_list = []
        slave_list = []
        accum_m_loff = 0
        accum_s_loff = 0

        for _ in range(MAX_QUEUE_ITEMS):
            try:
                m_codes, s_codes, m_loff, s_loff = self.data_queue.get_nowait()
                master_list.append(m_codes)
                slave_list.append(s_codes)
                accum_m_loff |= m_loff
                accum_s_loff |= s_loff
            except queue.Empty:
                break

        if not master_list:
            return

        all_m_codes = np.concatenate(master_list)
        all_s_codes = np.concatenate(slave_list)
        n_pts = len(all_m_codes)

        if n_pts == 0:
            return

        latest_m_code = int(all_m_codes[-1])
        latest_s_code = int(all_s_codes[-1])

        latest_m_mv = code_to_mv(latest_m_code)
        latest_s_mv = code_to_mv(latest_s_code)

        # Update Master Label (带消抖滤波)
        m_hex_str = f"0x{latest_m_code & 0xFFFFFF:06X}" if latest_m_code >= 0 else f"-0x{abs(latest_m_code) & 0xFFFFFF:06X}"
        self.master_code_val.setText(f"{latest_m_code}\n({m_hex_str})")
        self.master_mv_val.setText(f"{latest_m_mv:.3f} mV")

        filtered_m_loff = self.m_debouncer.update(accum_m_loff)
        m_loff_text, m_loff_color = format_loffstat(filtered_m_loff, is_slave=False)
        self.master_loff_val.setText(m_loff_text)
        self.master_loff_val.setStyleSheet(f"color: {m_loff_color}; font-size: 12px; font-weight: bold;")

        # Update Slave Label (带消抖滤波)
        s_hex_str = f"0x{latest_s_code & 0xFFFFFF:06X}" if latest_s_code >= 0 else f"-0x{abs(latest_s_code) & 0xFFFFFF:06X}"
        self.slave_code_val.setText(f"{latest_s_code}\n({s_hex_str})")
        self.slave_mv_val.setText(f"{latest_s_mv:.3f} mV")

        filtered_s_loff = self.s_debouncer.update(accum_s_loff)
        s_loff_text, s_loff_color = format_loffstat(filtered_s_loff, is_slave=True)
        self.slave_loff_val.setText(s_loff_text)
        self.slave_loff_val.setStyleSheet(f"color: {s_loff_color}; font-size: 12px; font-weight: bold;")

        # Update ring buffers (in-place rolling)
        if n_pts >= self.buffer_size:
            self.master_code_buf[:] = all_m_codes[-self.buffer_size:]
            self.slave_code_buf[:] = all_s_codes[-self.buffer_size:]
        else:
            self.master_code_buf[:-n_pts] = self.master_code_buf[n_pts:]
            self.master_code_buf[-n_pts:] = all_m_codes

            self.slave_code_buf[:-n_pts] = self.slave_code_buf[n_pts:]
            self.slave_code_buf[-n_pts:] = all_s_codes

        # Draw 6-lead curves
        self.update_plots()

    def update_plots(self):
        if self.plot_value_mode_combo.currentText() == "电压值 (mV)":
            v_i = (self.master_code_buf * VREF_MV) / (MAX_ADC_CODE * PGA_GAIN)
            v_ii = (self.slave_code_buf * VREF_MV) / (MAX_ADC_CODE * PGA_GAIN)
            y_unit = "mV"
        else:
            v_i = self.master_code_buf
            v_ii = self.slave_code_buf
            y_unit = "Code"

        # Apply real-time 1000Hz DSP Filtering if enabled
        if self.filter_enable_check.isChecked():
            hp = self.hp_check.isChecked()
            notch = self.notch_check.isChecked()
            lp = self.lp_check.isChecked()
            v_i = self.ecg_filter.process(v_i, hp=hp, notch=notch, lp=lp)
            v_ii = self.ecg_filter.process(v_ii, hp=hp, notch=notch, lp=lp)

        # Vector math to derive 6 limb leads:
        # Lead I = Master, Lead II = Slave
        v_iii = v_ii - v_i
        avr = -0.5 * (v_i + v_ii)
        avl = v_i - 0.5 * v_ii
        avf = v_ii - 0.5 * v_i

        lead_data = {
            'I': v_i,     'aVR': avr,
            'II': v_ii,   'aVL': avl,
            'III': v_iii, 'aVF': avf
        }

        is_autoscale = self.autoscale_check.isChecked()

        for name, data in lead_data.items():
            plot = self.plot_widgets[name]
            plot.setLabel('left', y_unit)
            self.curve_items[name].setData(data)
            if is_autoscale:
                plot.enableAutoRange(axis='y')
            else:
                if y_unit == "mV":
                    plot.setYRange(-2.5, 2.5)
                else:
                    plot.setYRange(-MAX_ADC_CODE, MAX_ADC_CODE)

    def on_plot_mode_changed(self):
        self.update_plots()

    def on_autoscale_changed(self):
        self.update_plots()

    def on_filter_changed(self):
        self.update_plots()

    @Slot(str)
    def append_log(self, text: str):
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        log_entry = f"[{timestamp}] {text}"
        self.log_text.append(log_entry)
        if self.autoscroll_check.isChecked():
            self.log_text.moveCursor(QTextCursor.MoveOperation.End)

        try:
            with open(LOG_FILE_PATH, "a", encoding="utf-8") as f:
                f.write(log_entry + "\n")
        except Exception:
            pass

    def clear_log(self):
        self.log_text.clear()

    def closeEvent(self, event):
        if self.ui_timer and self.ui_timer.isActive():
            self.ui_timer.stop()
        if self.receiver_thread and self.receiver_thread.isRunning():
            self.receiver_thread.stop()
        event.accept()


def main():
    multiprocessing.freeze_support()
    def slog(msg):
        print(f"[STARTUP] {msg}", flush=True)
        try:
            with open(STARTUP_LOG_PATH, "a", encoding="utf-8") as f:
                f.write(f"[{datetime.now()}] {msg}\n")
        except Exception:
            pass

    slog("1. Entering main()")
    app = QApplication(sys.argv)
    slog("2. QApplication created")
    app.setStyle('Fusion')
    slog("3. Style set, creating MainWindow")
    window = MainWindow()
    slog("4. MainWindow created, calling show()")
    window.show()
    slog("5. window.show() completed, entering app.exec()")
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
