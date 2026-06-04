#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
guoqie_io.py - 过切检查批处理 IO 层

文件系统操作、子进程管理、日志。无 NXOpen 依赖。
"""

from __future__ import annotations

import os
import shutil
import subprocess
from datetime import datetime
from typing import List, Tuple


def list_prt_files(input_dir: str) -> List[str]:
    if not os.path.isdir(input_dir):
        return []
    return sorted(
        os.path.join(input_dir, f)
        for f in os.listdir(input_dir)
        if f.lower().endswith('.prt')
    )


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def copy_to_target(src_prt: str, target_prt: str) -> None:
    parent = os.path.dirname(target_prt)
    if parent:
        os.makedirs(parent, exist_ok=True)
    shutil.copy2(src_prt, target_prt)


def move_file(src: str, dst_dir: str) -> None:
    ensure_dir(dst_dir)
    dst = os.path.join(dst_dir, os.path.basename(src))
    if os.path.exists(dst):
        os.remove(dst)
    shutil.move(src, dst)


def target_prt_path(input_prt: str, prt_dir: str) -> str:
    return os.path.join(prt_dir, os.path.basename(input_prt))


def json_path_for(prt_path: str) -> str:
    base = os.path.splitext(prt_path)[0]
    return base + "_data.json"


def open_worker_log(log_dir: str, worker_id: int):
    ensure_dir(log_dir)
    log_path = os.path.join(log_dir, f"worker_{worker_id}.log")
    log_f = open(log_path, 'w', encoding='utf-8')
    return log_f, log_path


def prt_log_path(log_dir: str, prt_full_path: str) -> str:
    base = os.path.splitext(os.path.basename(prt_full_path))[0]
    return os.path.join(log_dir, f"{base}.log")


def grep_log(log_path: str, keyword: str) -> List[str]:
    try:
        with open(log_path, 'r', encoding='utf-8', errors='replace') as f:
            return [l.rstrip() for l in f if keyword in l]
    except Exception:
        return []


def spawn_nx_worker(
    nx_run_journal: str,
    worker_script: str,
    log_f,
    cwd: str,
    env_vars: dict,
) -> subprocess.Popen:
    full_env = os.environ.copy()
    full_env.update(env_vars)
    return subprocess.Popen(
        [nx_run_journal, worker_script],
        stdout=log_f,
        stderr=subprocess.STDOUT,
        cwd=cwd,
        env=full_env,
    )


def count_json_files(json_dir: str) -> int:
    if not os.path.isdir(json_dir):
        return 0
    return len([f for f in os.listdir(json_dir) if f.endswith('_data.json')])


def collect_worker_summaries(workers: List[Tuple]) -> List[str]:
    summary = []
    for wid, _p, _f, log_path in workers:
        tail = grep_log(log_path, 'DONE')
        if tail:
            summary.append(f"  W{wid}: {tail[-1]}")
        else:
            summary.append(f"  W{wid}: (无 DONE 记录)")
    return summary
