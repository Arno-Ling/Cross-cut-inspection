NX CAM 过切检查并行批处理工具
====================================

项目结构：
  guoqiejiancha.cpp       - C++ DLL 源码（NX Open API）
  guoqiejiancha.sln       - Visual Studio 解决方案
  guoqiejiancha.vcxproj   - VS 项目文件
  scripts/                - Python 调度脚本

编译方式：
  1. 双击 guoqiejiancha.sln，用 VS 2022 打开
  2. 选择 Release | x64
  3. 生成 → 生成解决方案
  DLL 自动复制到 scripts\guoqiejiancha.dll

使用方式：
  cd scripts
  python guoqie_batch.py <input_dir> <output_dir>

NX 手动执行：
  文件 → 执行 → NX Open → 选择 scripts\guoqiejiancha.dll
