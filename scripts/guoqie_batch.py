#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
guoqie_batch.py - 过切检查并行批处理 入口

输出结构（output_dir 下）：
  output_dir/prt/   生成刀路后的 PRT 文件
  output_dir/json/  工序参数 JSON
  output_dir/logs/  worker 日志

命令行用法：
  python guoqie_batch.py <input_dir> <output_dir>
  python guoqie_batch.py                   （从 guoqie_paths.cfg 读取）

编程调用：
  from guoqie_batch import main
  main(input_dir=r"input", output_dir=r"output")
"""

from __future__ import annotations

import logging
import os
import sys
import traceback
from pathlib import Path
from typing import Dict

from guoqie_execution import main_dispatcher


# =============================================================================
# 可修改配置（直接编辑此处）
# =============================================================================
# 相对路径基于项目根目录（过切检查/）解析

INPUT_DIR = r"input"                        # 输入 PRT 目录
OUTPUT_DIR = r"output"                       # 输出根目录
DLL_PATH = r"x64\Release\guoqiejiancha.dll"      # Release|x64 DLL
NX_RUN_JOURNAL = r"C:\Program Files\Siemens\NX2312\NXBIN\run_journal.exe"
DEFAULT_WORKERS = 4
# =============================================================================


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, ".."))


def _resolve(path: str) -> str:
    if not path:
        return ""
    if os.path.isabs(path):
        return os.path.normpath(path)
    return os.path.normpath(os.path.join(PROJECT_ROOT, path))


# -----------------------------------------------------------------------------
# 配置文件读取（guoqie_paths.cfg），覆盖上方硬编码默认值
# -----------------------------------------------------------------------------
CFG_FILENAME = "guoqie_paths.cfg"


def load_cfg() -> Dict[str, str]:
    cfg_path = os.path.join(SCRIPT_DIR, CFG_FILENAME)
    result: Dict[str, str] = {}
    if not os.path.isfile(cfg_path):
        return result
    with open(cfg_path, "r", encoding="utf-8-sig") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip()
            if len(val) >= 2 and val[0] == val[-1] and val[0] in ('"', "'"):
                val = val[1:-1]
            if key:
                result[key] = val
    return result


def apply_cfg(cfg: Dict[str, str]) -> tuple:
    inp = cfg.get("INPUT_DIR") or INPUT_DIR
    out = cfg.get("OUTPUT_DIR") or OUTPUT_DIR
    dll = cfg.get("DLL_PATH") or DLL_PATH
    nx  = cfg.get("NX_RUN_JOURNAL") or NX_RUN_JOURNAL
    n   = int(cfg.get("DEFAULT_WORKERS") or str(DEFAULT_WORKERS))
    return _resolve(inp), _resolve(out), _resolve(dll), nx, n


# -----------------------------------------------------------------------------
# 日志
# -----------------------------------------------------------------------------
def setup_logger(name: str) -> logging.Logger:
    logger = logging.getLogger(name)
    logger.setLevel(logging.INFO)
    logger.handlers.clear()
    formatter = logging.Formatter("%(asctime)s | %(levelname)s | %(message)s")
    log_file = os.getenv("SKILL_LOG_FILE", "").strip()
    if log_file:
        Path(log_file).parent.mkdir(parents=True, exist_ok=True)
        fh = logging.FileHandler(log_file, encoding="utf-8")
        fh.setFormatter(formatter)
        logger.addHandler(fh)
    sh = logging.StreamHandler()
    sh.setFormatter(formatter)
    logger.addHandler(sh)
    return logger


logger = setup_logger("guoqie_batch")


# -----------------------------------------------------------------------------
# 主入口（编程调用）
# -----------------------------------------------------------------------------
def main(input_dir: str, output_dir: str) -> int:
    in_dir  = os.path.abspath(input_dir)
    out_dir = os.path.abspath(output_dir)

    worker_script = os.path.join(SCRIPT_DIR, "guoqie_execution.py")
    log_dir       = os.path.join(out_dir, "logs")

    # DLL 路径：先相对 scripts/ 找，再相对项目根找
    dll_path = os.path.join(SCRIPT_DIR, "guoqiejiancha.dll")
    if not os.path.exists(dll_path):
        dll_path = _resolve(DLL_PATH)

    print("=" * 60)
    print(f"  NX CAM 过切检查并行批处理 - {DEFAULT_WORKERS} 进程")
    print("=" * 60)
    print(f"输入目录:  {in_dir}")
    print(f"输出目录:  {out_dir}")
    print(f"DLL 路径:  {dll_path}")
    print(f"日志目录:  {log_dir}\n")

    logger.info(
        f"启动批处理: workers={DEFAULT_WORKERS}, "
        f"input={in_dir}, output={out_dir}"
    )

    try:
        rc = main_dispatcher(
            n=DEFAULT_WORKERS,
            input_dir=in_dir,
            output_dir=out_dir,
            nx_run_journal=NX_RUN_JOURNAL,
            worker_script=worker_script,
            log_dir=log_dir,
            cwd=SCRIPT_DIR,
            dll_path=dll_path,
        )
        if rc == 0:
            logger.info("批处理完成")
        else:
            logger.error(f"批处理失败 rc={rc}")
        return rc
    except Exception as e:
        logger.error(f"批处理异常: {e}")
        traceback.print_exc()
        return 1


# -----------------------------------------------------------------------------
# CLI 入口
# -----------------------------------------------------------------------------
def _print_usage_and_exit() -> None:
    print("用法:")
    print("  python guoqie_batch.py <input_dir> <output_dir>")
    print("  python guoqie_batch.py          （从 guoqie_paths.cfg 读取目录）")
    print()
    print("首次使用前：")
    print("  1. 打开 guoqiejiancha.sln → Release | x64 → 生成解决方案")
    print("  2. DLL 自动复制到 scripts\\guoqiejiancha.dll")
    print()
    print("路径配置：编辑 scripts\\guoqie_paths.cfg 或直接修改 guoqie_batch.py 顶部")
    sys.exit(1)


if __name__ == "__main__":
    cfg = load_cfg()

    if len(sys.argv) == 3:
        inp, out = sys.argv[1], sys.argv[2]
    elif len(sys.argv) == 1:
        inp, out, *_ = apply_cfg(cfg)
        if not inp or not out:
            _print_usage_and_exit()
    else:
        _print_usage_and_exit()

    sys.exit(main(input_dir=inp, output_dir=out))
