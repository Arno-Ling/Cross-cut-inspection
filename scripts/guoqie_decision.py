#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
guoqie_decision.py - 常量与业务逻辑（无 IO，无 NXOpen 依赖）
"""

from __future__ import annotations

import os
from typing import List, Tuple


DEFAULT_WORKERS = 3
STAGGER_SECONDS = 5
POLL_INTERVAL_SECONDS = 2
STATUS_PRINT_INTERVAL_SECONDS = 30


def split_files_for_worker(
    all_files: List[str],
    worker_id: int,
    total_workers: int,
) -> List[str]:
    if total_workers <= 0 or worker_id < 0 or worker_id >= total_workers:
        return []
    return [f for i, f in enumerate(all_files) if i % total_workers == worker_id]


def validate_environment(
    nx_run_journal: str,
    worker_script: str,
    input_dir: str,
    dll_path: str,
) -> Tuple[bool, str]:
    if not os.path.exists(nx_run_journal):
        return False, f"run_journal.exe 找不到: {nx_run_journal}"
    if not os.path.exists(worker_script):
        return False, f"worker 脚本找不到: {worker_script}"
    if not os.path.isdir(input_dir):
        return False, f"输入目录不存在: {input_dir}"
    if not os.path.exists(dll_path):
        return False, f"DLL 不存在: {dll_path}"
    return True, ""


def estimate_remaining(
    done_count: int,
    total_count: int,
    elapsed_sec: float,
) -> float:
    if done_count <= 0 or total_count <= 0:
        return 0.0
    rate = done_count / elapsed_sec if elapsed_sec > 0 else 0.0
    if rate <= 0:
        return 0.0
    remaining_count = max(total_count - done_count, 0)
    return remaining_count / rate
