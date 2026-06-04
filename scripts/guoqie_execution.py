#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
guoqie_execution.py - 过切检查执行层

双角色：
- 主控端：启动 N 个 NX worker 进程并监控完成
- Worker 端（NX 子进程内）：加载 DLL，打开/关闭 PRT，调用 DLL 处理

输出结构（output_dir 下）：
  output_dir/prt/   生成刀路后的 PRT 文件
  output_dir/json/  工序参数 JSON
  output_dir/logs/  worker 日志
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import time
from typing import Callable, List, Tuple

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if _SCRIPT_DIR not in sys.path:
    sys.path.insert(0, _SCRIPT_DIR)

from guoqie_decision import (
    POLL_INTERVAL_SECONDS,
    STAGGER_SECONDS,
    STATUS_PRINT_INTERVAL_SECONDS,
    estimate_remaining,
    split_files_for_worker,
    validate_environment,
)
from guoqie_io import (
    copy_to_target,
    count_json_files,
    ensure_dir,
    json_path_for,
    list_prt_files,
    move_file,
    prt_log_path,
    spawn_nx_worker,
    target_prt_path,
)

try:
    import NXOpen
    HAS_NXOPEN = True
except ImportError:
    HAS_NXOPEN = False


# =============================================================================
# DLL 加载
# =============================================================================
def load_dll(dll_path: str):
    if not os.path.exists(dll_path):
        raise FileNotFoundError(f"DLL not found: {dll_path}")
    dll = ctypes.CDLL(dll_path)
    fn = getattr(dll, "run_gouge_check")
    fn.argtypes = [ctypes.c_char_p]
    fn.restype = ctypes.c_int
    return dll, fn


# =============================================================================
# NX 部件操作
# =============================================================================
def open_part(session, part_path: str, worker_id: int) -> bool:
    try:
        session.Parts.OpenBaseDisplay(part_path)
        return session.Parts.Work is not None
    except Exception as e:
        print(f"  [W{worker_id}] [ERROR] open: {e}")
        return False


def close_work_part(session) -> None:
    try:
        wp = session.Parts.Work
        if wp is not None:
            wp.Close(NXOpen.BasePart.CloseWholeTree.True_,
                     NXOpen.BasePart.CloseModified.CloseModified, None)
    except Exception:
        try:
            session.Parts.CloseAll(
                NXOpen.BasePart.CloseModified.CloseModified, None)
        except Exception:
            pass


# =============================================================================
# Worker 端：单 PRT 处理
# =============================================================================
def process_one_part(
    session,
    dll_fn: Callable,
    part_path: str,
    prt_dir: str,
    json_dir: str,
    log_dir: str,
    worker_id: int,
) -> bool:
    name = os.path.basename(part_path)
    prt_log = prt_log_path(log_dir, part_path)
    t_start = time.time()

    def prt_log_write(msg: str):
        t = time.strftime("%H:%M:%S", time.localtime())
        with open(prt_log, 'a', encoding='utf-8') as f:
            f.write(f"[{t}] {msg}\n")

    prt_log_write(f"开始处理: {name}")
    print(f"[W{worker_id}] {name}")

    target = target_prt_path(part_path, prt_dir)
    try:
        copy_to_target(part_path, target)
        prt_log_write(f"复制完成: {target}")
    except Exception as e:
        prt_log_write(f"复制失败: {e}")
        print(f"  [W{worker_id}] [ERROR] copy: {e}")
        return False

    if not open_part(session, target, worker_id):
        prt_log_write("NX 打开失败")
        return False
    prt_log_write("NX 打开成功")

    try:
        try:
            session.ApplicationSwitchImmediate("UG_APP_MANUFACTURING")
        except Exception:
            pass

        rc = dll_fn(b"")
        elapsed = time.time() - t_start

        if rc == 0:
            src_json = json_path_for(target)
            if os.path.exists(src_json):
                move_file(src_json, json_dir)
                prt_log_write(f"JSON 已移动: {os.path.basename(src_json)}")
            prt_log_write(f"完成 ({elapsed:.1f}s)")
            print(f"[W{worker_id}]   OK ({elapsed:.1f}s) -> {target}")
            return True
        else:
            prt_log_write(f"DLL 返回错误码: {rc}")
            print(f"[W{worker_id}]   FAIL rc={rc} ({elapsed:.1f}s)")
            return False
    finally:
        close_work_part(session)


# =============================================================================
# Worker 端：主循环
# =============================================================================
def run_worker() -> None:
    input_dir   = os.environ.get("GUOQIE_INPUT", "")
    output_dir  = os.environ.get("GUOQIE_OUTPUT", "")
    dll_path    = os.environ.get("GUOQIE_DLL_PATH", "")
    worker_id   = int(os.environ.get("GUOQIE_WID", "0"))
    total       = int(os.environ.get("GUOQIE_TOTAL", "1"))

    if not input_dir or not output_dir or not dll_path:
        print("[ERROR] 必需环境变量未设置 (GUOQIE_INPUT, GUOQIE_OUTPUT, GUOQIE_DLL_PATH)")
        return

    prt_dir  = os.path.join(output_dir, "prt")
    json_dir = os.path.join(output_dir, "json")
    log_dir  = os.path.join(output_dir, "logs")

    print(f"[W{worker_id}] Input={input_dir}")
    print(f"[W{worker_id}] Output={output_dir}")
    print(f"[W{worker_id}] DLL={dll_path}")

    if not HAS_NXOPEN:
        print(f"[W{worker_id}] [ERROR] 必须在 NX 环境内运行")
        return
    if not os.path.isdir(input_dir):
        print(f"[W{worker_id}] [ERROR] 输入目录不存在")
        return

    ensure_dir(prt_dir)
    ensure_dir(json_dir)
    ensure_dir(log_dir)

    print(f"[W{worker_id}] DLL: {dll_path}")
    try:
        _dll, fn = load_dll(dll_path)
    except Exception as e:
        print(f"[W{worker_id}] [ERROR] DLL: {e}")
        return

    all_files = list_prt_files(input_dir)
    my_files = split_files_for_worker(all_files, worker_id, total)
    print(f"[W{worker_id}] 分配到 {len(my_files)} 个文件（共 {len(all_files)} 个）")
    if not my_files:
        return

    session = NXOpen.Session.GetSession()
    ok = fail = 0
    t0 = time.time()
    for i, prt in enumerate(my_files, 1):
        print(f"\n[W{worker_id}] [{i}/{len(my_files)}]", end=" ")
        if process_one_part(session, fn, prt, prt_dir, json_dir, log_dir, worker_id):
            ok += 1
        else:
            fail += 1

    elapsed = time.time() - t0
    print(f"\n[W{worker_id}] DONE in {elapsed:.1f}s. OK: {ok}  Failed: {fail}")


# =============================================================================
# 主控端：N 进程并行调度
# =============================================================================
def start_workers(
    n: int,
    nx_run_journal: str,
    worker_script: str,
    cwd: str,
    log_dir: str,
    input_dir: str,
    output_dir: str,
    dll_path: str,
) -> List[Tuple]:
    workers = []
    for i in range(n):
        env = {
            "GUOQIE_INPUT":      input_dir,
            "GUOQIE_OUTPUT":     output_dir,
            "GUOQIE_DLL_PATH":   dll_path,
            "GUOQIE_WID":        str(i),
            "GUOQIE_TOTAL":      str(n),
        }
        print(f"  启动 Worker {i}")
        p = spawn_nx_worker(nx_run_journal, worker_script, subprocess.DEVNULL, cwd, env)
        workers.append((i, p))
        if i < n - 1:
            time.sleep(STAGGER_SECONDS)
    return workers


def monitor_workers(
    workers: List[Tuple],
    json_dir: str,
    total_files: int,
) -> float:
    t_start = time.time()
    finished = [False] * len(workers)
    last_print = 0.0

    while not all(finished):
        for idx, (wid, p) in enumerate(workers):
            if finished[idx]:
                continue
            rc = p.poll()
            if rc is not None:
                finished[idx] = True
                el = time.time() - t_start
                print(f"\n[+{el:.0f}s] Worker {wid} 完成 (rc={rc})")

        elapsed = time.time() - t_start
        if elapsed - last_print >= STATUS_PRINT_INTERVAL_SECONDS:
            last_print = elapsed
            running = sum(1 for f in finished if not f)
            done = count_json_files(json_dir)
            remain_sec = estimate_remaining(done, total_files, elapsed)
            etr = f"，预计还需 {remain_sec/60:.1f} 分钟" if remain_sec > 0 else ""
            print(f"  [+{elapsed:.0f}s] {running}/{len(workers)} 进程跑着，"
                  f"已完成 {done}/{total_files}{etr}")

        time.sleep(POLL_INTERVAL_SECONDS)

    return time.time() - t_start


def main_dispatcher(
    n: int,
    input_dir: str,
    output_dir: str,
    nx_run_journal: str,
    worker_script: str,
    log_dir: str,
    cwd: str,
    dll_path: str,
) -> int:
    ok, msg = validate_environment(nx_run_journal, worker_script, input_dir, dll_path)
    if not ok:
        print(f"[ERROR] {msg}")
        return 1

    prts = list_prt_files(input_dir)
    if not prts:
        print(f"[ERROR] 输入目录里没有 .prt 文件: {input_dir}")
        return 1

    print(f"找到 {len(prts)} 个 .prt 文件")
    if len(prts) < n:
        print(f"[INFO] 文件数 ({len(prts)}) 少于进程数 ({n})，部分进程会空闲")

    prt_dir  = os.path.join(output_dir, "prt")
    json_dir = os.path.join(output_dir, "json")
    ensure_dir(prt_dir)
    ensure_dir(json_dir)
    ensure_dir(log_dir)

    print(f"\n启动 {n} 个 NX 进程...")
    workers = start_workers(
        n, nx_run_journal, worker_script, cwd, log_dir,
        input_dir, output_dir, dll_path,
    )

    print(f"\n全部启动完毕。\n")

    total_time = monitor_workers(workers, json_dir, len(prts))

    print("\n" + "=" * 60)
    print(f"  全部完成 - 总耗时 {total_time:.0f}s ({total_time/60:.1f} 分钟)")
    print("=" * 60)
    print(f"输出: {output_dir}")
    print(f"  PRT: {prt_dir}")
    print(f"  JSON: {json_dir}")
    print(f"  日志: {log_dir}\n")

    return 0


if __name__ == "__main__":
    run_worker()
