# NX CAM 过切检查并行批处理工具

对一批 PRT 文件**并行**执行 NX CAM 过切检查（生成刀路 -> 过切检查 -> 工序参数 JSON），由 **C++ DLL（NX Open API）+ Python 调度器**实现。

> 须在已安装 NX（默认 NX2312）的 Windows 10/11 环境下运行。并行处理需要对应数量的 NX CAM 许可证。

---

## 目录结构

```text
过切检查/
├── README.md
├── guoqiejiancha.cpp            # C++ DLL 源码
├── guoqiejiancha.sln            # Visual Studio 解决方案
├── guoqiejiancha.vcxproj        # VS 项目文件
├── input/                       # 输入 PRT 文件
├── output/                      # 处理结果输出
└── scripts/
    ├── guoqie_batch.py          # 入口：CLI + 编程调用
    ├── guoqie_execution.py      # NX 处理 + 并行调度
    ├── guoqie_decision.py       # 常量 + 文件分包
    ├── guoqie_io.py             # 文件系统 + 子进程
    ├── guoqie_config.py         # 配置文件读取
    ├── guoqie_paths.cfg         # 运行配置
    └── parallel_logs/           # worker 日志
```

---

## 编译 DLL

1. 双击 `guoqiejiancha.sln` 用 Visual Studio 2022 打开
2. 顶部选择 `Release | x64`
3. 点击 **生成** → **生成解决方案**
4. 编译成功后 DLL 自动复制到 `scripts\guoqiejiancha.dll`

> NX SDK 路径：若 NX 不在 `C:\Program Files\Siemens\NX2312`，请设置环境变量 `UGII_BASE_DIR` 指向 NX 安装目录，或在 VS 中修改项目属性（UserMacros → NXBaseDir）。

---

## 运行

### 方式一：命令行（推荐）
```bash
cd scripts
python guoqie_batch.py ..\input ..\output
```

### 方式二：Python 代码调用
```python
import sys
sys.path.insert(0, r"scripts")

from guoqie_batch import main
main(input_dir=r"input", output_dir=r"output")
```

### 方式三：NX 手动执行
- 在 NX 中打开 PRT 文件
- **文件** → **执行** → **NX Open** → 选择 `scripts\guoqiejiancha.dll`
- DLL 会对当前 work part 执行: 刀路生成 → 过切检查 → JSON 导出 → 就地保存

---

## 输出结构

```text
output/
├── {零件名1}/
│   ├── {零件名1}.prt            # 处理后部件副本
│   └── {零件名1}_data.json      # 工序参数 JSON
├── {零件名2}/
│   └── ...
```

---

## 配置

编辑 `scripts/guoqie_paths.cfg`：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `INPUT_DIR` | `..\input` | 输入目录 |
| `OUTPUT_DIR` | `..\output` | 输出目录 |
| `DEFAULT_WORKERS` | `3` | 并行 NX 进程数 |
| `NX_RUN_JOURNAL` | `C:\Program Files\Siemens\NX2312\NXBIN\run_journal.exe` | NX 启动器 |

---

## 返回值

| 接口 | 返回值 |
|------|--------|
| `guoqie_batch.py`（CLI） | 退出码 0=成功，1=失败 |
| `main()` | `int` 退出码 |
| DLL `run_gouge_check()` | 0=成功，-1=无工作部件，-2=异常 |
