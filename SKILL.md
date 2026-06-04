---
name: guoqie_batch_parallel
description: 对一批 PRT 文件并行执行 NX CAM 过切检查（生成刀路 + 过切检查 + 工序参数 JSON），通过 C++ DLL + Python 调度器实现。须在已安装 NX 的 Windows 环境下运行。
entry_script: scripts/guoqie_batch.py
param_order:
  - INPUT_DIR
  - OUTPUT_DIR
---
## Overview

此 Skill 用于 **NX CAM 过切检查批量并行处理**。底层是一个 C++ 编译的 NX Open DLL（`guoqiejiancha.dll`），Python 调度器把输入目录里所有 `.prt` 文件平均分给 N 个并行 NX 进程，每个进程内 worker 串行处理它分到的文件。每个 PRT 单独输出处理后的 PRT 副本和工序参数 JSON。

> 进程数受 NX license 数量与系统内存共同限制；建议在 6 核 16GB 机器上 N=3~6。

| 项目 | 说明 |
| --- | --- |
| 处理引擎 | C++ DLL（NX Open API）|
| 并行模型 | N 个 `run_journal.exe` 子进程，进程间互不干扰 |
| 单 worker | 串行处理它分到的 PRT 子集 |
| 文件分包 | 按 `index % N == worker_id` 平均分配 |
| 处理流水线 | 刀路 → 过切检查 → JSON |
| 交付物 | `output/prt/{零件名}.prt` + `output/json/{零件名}_data.json` |

- 入口脚本：`scripts/guoqie_batch.py`
- 对外参数：**只有 input_dir 和 output_dir 两个**
- 其它配置（进程数 / NX 路径 / DLL 路径）直接编辑 `guoqie_batch.py` 顶部修改

依赖环境：
- 安装 NX2312（默认路径 `C:\Program Files\Siemens\NX2312`）
- 已编译的 `guoqiejiancha.dll`（VS 输出到 `x64/Release/`，Post-Build 自动复制到 `scripts/`）
- 充足的 NX CAM license（每进程占用一个）

运行模式：
- **命令行**：`python guoqie_batch.py <input> <output>`
- **无参**：使用 `guoqie_batch.py` 顶部硬编码默认路径
- **编程调用**：`from guoqie_batch import main; main(input_dir=..., output_dir=...)`

## Project Structure

```text
过切检查/
├── SKILL.md                           # 本文件
├── guoqiejiancha.cpp                  # C++ DLL 源码
├── guoqiejiancha.sln                  # Visual Studio 解决方案
├── guoqiejiancha.vcxproj              # VS 项目文件
├── input/                             # 输入 PRT 目录
├── output/                            # 输出根目录
│   ├── prt/                           # 处理后 PRT 文件
│   ├── json/                          # 工序参数 JSON
│   └── logs/                          # worker 日志
├── x64/Release/                       # VS 编译输出
│   └── guoqiejiancha.dll
└── scripts/
    ├── guoqie_batch.py                # 入口：CLI + 编程调用
    ├── guoqie_decision.py             # 常量、文件分包、环境校验
    ├── guoqie_execution.py            # Worker（NX 进程内）+ 主控调度
    └── guoqie_io.py                   # 文件系统、子进程、日志
```

DLL 编译后在 VS Post-Build 中自动复制到 `scripts/guoqiejiancha.dll`，Python 直接使用此路径。

## Quick Start

### 1) 首次准备

- 打开 `guoqiejiancha.sln`，选择 `Release | x64`，**生成 → 生成解决方案**
- DLL 自动复制到 `scripts/guoqiejiancha.dll`
- 确认 `input/` 目录下有至少一个 `.prt` 文件

### 2) 命令行（使用默认路径）

```bash
cd "过切检查/scripts"
python guoqie_batch.py
```

使用 `guoqie_batch.py` 顶部 `INPUT_DIR` / `OUTPUT_DIR` 的硬编码默认值。

### 3) 命令行（手动指定目录）

```bash
cd "过切检查/scripts"
python guoqie_batch.py C:\path\to\input C:\path\to\output
```

### 4) Python 代码调用

```python
import sys
sys.path.insert(0, r"C:\...\过切检查\scripts")

from guoqie_batch import main

main(
    input_dir=r"C:\...\过切检查\input",
    output_dir=r"C:\...\过切检查\output",
)
```

底层由 `main_dispatcher` → `start_workers` 启动 N 个 `run_journal.exe`，每个 NX 进程跑 `guoqie_execution.py`（`__main__` → `run_worker()` → `process_one_part()`）→ 加载 DLL 处理它分到的 PRT。

## Parameters

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `INPUT_DIR` | 是* | 含 `.prt` 文件的输入目录 |
| `OUTPUT_DIR` | 是* | 输出根目录（自动创建 `prt/` `json/` `logs/`）|

\* 未传参时使用 `guoqie_batch.py` 顶部的硬编码默认值。

其它配置（`guoqie_batch.py` 顶部）：

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `DLL_PATH` | `scripts\guoqiejiancha.dll` | DLL 路径（相对项目根）|
| `NX_RUN_JOURNAL` | `C:\Program Files\Siemens\NX2312\NXBIN\run_journal.exe` | NX 启动器 |
| `DEFAULT_WORKERS` | `3` | 并行 NX 进程数 |

## Output Layout

```text
{OUTPUT_DIR}/
├── prt/
│   ├── {零件名1}.prt            # 处理后部件副本（含刀路 / 过切结果）
│   └── {零件名2}.prt
├── json/
│   ├── {零件名1}_data.json      # 工序参数 JSON（toolpath time 等）
│   └── {零件名2}_data.json
└── logs/
    ├── {零件名1}.log            # per-PRT 处理日志
    ├── {零件名2}.log
    ├── worker_0.log             # worker 原始输出
    └── worker_1.log
```

## Return Values

| 接口 | 返回 |
| --- | --- |
| `guoqie_batch.py`（CLI）| 成功 `sys.exit(0)`，失败 `sys.exit(1)` |
| `main(input_dir, output_dir)` | `int` 退出码（0=成功，1=失败/异常）|
| `main_dispatcher(...)` | `int` 退出码 |
| `run_worker()` | 无返回值；NX 内部由 `guoqie_execution.py` 的 `__main__` 调用 |
| DLL `run_gouge_check()` | C int：0=成功，-1=无工作部件，-2=异常 |

## 建议执行顺序

```
1. 把 .prt 文件放到 input/ 目录
2. 编辑 `guoqie_batch.py` 顶部 `DEFAULT_WORKERS`（先小并行度试一次）
3. cd scripts && python guoqie_batch.py
4. 观察 output/logs/ 下的日志，确认 license/内存够
5. 加大 DEFAULT_WORKERS（4 或 6），跑全量
```

实时监控某个 worker 进度：

```powershell
Get-Content output\logs\worker_0.log -Wait -Tail 20
```

## Preconditions

- Windows 10/11，已安装 NX2312（默认路径 `C:\Program Files\Siemens\NX2312`）
- `guoqiejiancha.dll` 已编译（VS 打开 `guoqiejiancha.sln`，`Release | x64` → 生成）
- NX CAM license 数量 &ge; `DEFAULT_WORKERS`
- 系统内存 &ge; 2GB &times; `DEFAULT_WORKERS`
- 输入目录里有 `.prt` 文件
- PRT 已配置 CAM 工序（否则 DLL 报 "No CAMSetup"）

## Common Failures

| 现象 | 可能原因 | 解决 |
| --- | --- | --- |
| `ModuleNotFoundError: NXOpen`（worker 日志）| 没在 NX Python 环境运行 | 通过 `run_journal.exe` 启动，不要直接 `python guoqie_execution.py` |
| `run_journal.exe 找不到` | NX 路径不对 | 修改 `guoqie_batch.py` 顶部的 `NX_RUN_JOURNAL` |
| worker 日志全 0 字节，但 NX 进程存在 | NX 启动慢 / license 排队 | 等 30-90 秒；或减小 `DEFAULT_WORKERS` |
| 部分 worker 立即 rc=1 退出 | license 不足 | 减小 `DEFAULT_WORKERS`；或检查 license server |
| 内存不够 NX 崩溃 | 进程数 &times; 2GB &gt; 物理内存 | 降低 `DEFAULT_WORKERS` |
| 退出码 `1` | 路径错误或 NX 异常 | 查 stderr / `output/logs/` 下的日志 |
| DLL 找不到 | 忘记编译或 Post-Build 失败 | 在 VS 中重新生成，确认 `scripts/guoqiejiancha.dll` 存在 |

## Logging

- 入口：`setup_logger("guoqie_batch")`
- 默认输出到 **stderr**（StreamHandler）
- 环境变量 `SKILL_LOG_FILE`：设置后额外落盘到该路径
- 各 worker 原始输出：`output/logs/worker_{N}.log`
- 各 PRT 处理日志：`output/logs/{零件名}.log`
- DLL 内部日志：通过 `OutputDebugStringA` 输出到 Visual Studio Output 窗口（调试用）

## Related / Downstream

| 模块 | 关系 |
| --- | --- |
| `guoqiejiancha.cpp` | 本 Skill 调用的 DLL 源码 |
| `guoqiejiancha.sln` / `guoqiejiancha.vcxproj` | VS 项目，重新编译 DLL 用 |
| `guoqie_batch.py` 顶部变量 | 运行时路径配置 |

本 Skill 只负责 **批量并行的过切检查处理**；单文件调试请直接在 NX 中通过 **文件 → 执行 → NX Open** 加载 `scripts/guoqiejiancha.dll`。
