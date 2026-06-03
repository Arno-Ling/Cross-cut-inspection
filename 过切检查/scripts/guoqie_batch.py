#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
guoqie_batch.py - 过切检查并行批处理 入口脚本

对外只暴露两个参数：input_dir 和 output_dir。
其它配置（workers / mode / NX 路径等）全部退化为 guoqie_decision.py 顶部的常量。
要调整时改 guoqie_decision.py 的常量块即可。

代码分层：
  guoqie_io        : 文件系统、子进程、日志重定向
  guoqie_decision  : 模块常量、文件分包、参数校验、估算
  guoqie_execution : worker 端 NX 处理 + 主控端调度
  guoqie_batch     : 本文件，CLI 入口 + 日志初始化

命令行用法：
    python guoqie_batch.py <input_dir> <output_dir>

编程调用：
    from guoqie_batch import main
    main(input_dir=r"...\\input", output_dir=r"...\\output")

日志：默认输出 stderr。设置环境变量 SKILL_LOG_FILE 可额外落盘。
"""

from __future__ import annotations

import logging
import os
import sys
import traceback
from pathlib import Path

from guoqie_decision import (
    DEFAULT_MODE,
    DEFAULT_WORKERS,
    LOG_SUBDIR,
    NX_RUN_JOURNAL,
)   
from guoqie_execution import main_dispatcher
from guoqie_io import to_abs_path

__all__ = ["main"]


# -----------------------------------------------------------------------------
# 日志初始化（stderr + 可选文件）
# -----------------------------------------------------------------------------
def setup_skill_logger(default_name: str) -> logging.Logger:
    skill_logger = logging.getLogger(default_name)
    skill_logger.setLevel(logging.INFO)
    skill_logger.handlers.clear()
    formatter = logging.Formatter(
        "%(asctime)s | %(levelname)s | %(name)s | %(message)s"
    )
    log_file = os.getenv("SKILL_LOG_FILE", "").strip()
    if log_file:
        log_path = Path(log_file)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        fh = logging.FileHandler(log_path, encoding="utf-8")
        fh.setFormatter(formatter)
        skill_logger.addHandler(fh)
    sh = logging.StreamHandler()
    sh.setFormatter(formatter)
    skill_logger.addHandler(sh)
    return skill_logger


logger = setup_skill_logger("skill.guoqie_batch")
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


# -----------------------------------------------------------------------------
# 主入口（编程调用）
# -----------------------------------------------------------------------------
def main(input_dir: str, output_dir: str) -> int:
    """
    并行批处理主入口。

    参数:
        input_dir   含 .prt 文件的输入目录
        output_dir  输出根目录（每个 PRT 一个子目录）

    返回:
        退出码（0 = 成功，1 = 失败）
    """
    in_dir  = to_abs_path(input_dir)
    out_dir = to_abs_path(output_dir)

    worker_script = os.path.join(SCRIPT_DIR, "guoqie_execution.py")
    log_dir       = os.path.join(SCRIPT_DIR, LOG_SUBDIR)

    print("=" * 60)
    print(f"  NX CAM 过切检查并行批处理 - {DEFAULT_WORKERS} 进程，{DEFAULT_MODE} 模式")
    print("=" * 60)
    print(f"输入目录: {in_dir}")
    print(f"输出目录: {out_dir}")
    print(f"日志目录: {log_dir}\n")

    logger.info(
        f"启动并行批处理: workers={DEFAULT_WORKERS}, mode={DEFAULT_MODE}, "
        f"input={in_dir}, output={out_dir}"
    )

    try:
        rc = main_dispatcher(
            n=DEFAULT_WORKERS,
            input_dir=in_dir,
            output_dir=out_dir,
            mode=DEFAULT_MODE,
            nx_run_journal=NX_RUN_JOURNAL,
            worker_script=worker_script,
            log_dir=log_dir,
            cwd=SCRIPT_DIR,
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
    print("用法: python guoqie_batch.py <input_dir> <output_dir>")
    print()
    print("  input_dir   含 .prt 文件的输入目录")
    print("  output_dir  输出根目录（每个 PRT 一个子目录）")
    print()
    print("其它配置（进程数 / 模式 / NX 路径）请编辑 guoqie_decision.py 顶部常量。")
    sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        _print_usage_and_exit()
    sys.exit(main(input_dir=sys.argv[1], output_dir=sys.argv[2]))
        return 1


# -----------------------------------------------------------------------------
# CLI 入口
# -----------------------------------------------------------------------------
if __name__ == "__main__":
    try:
        if len(sys.argv) > 1:
            n, in_d, out_d, mode = parse_cli_args(
                sys.argv[1:], RUN_INPUT, RUN_OUTPUT
            )
            sys.exit(main(workers=n, input_dir=in_d, output_dir=out_d, mode=mode))
        else:
            sys.exit(main())
    except ValueError as e:
        print(f"[ERROR] 参数错误: {e}")
        sys.exit(1)
