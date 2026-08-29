"""
LH001-91 6-Limb-Lead ECG Viewer Workstation Wrapper
Imports and runs the fused MainWindow from main.py
"""

import sys
import os

# Ensure local directory is in path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from main import main, MainWindow, ECGSimulator

if __name__ == "__main__":
    main()
