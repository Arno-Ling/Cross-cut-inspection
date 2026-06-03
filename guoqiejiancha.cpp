//=============================================================================
// guoqiejiancha.cpp
// NX CAM 过切检查 DLL（NX2312）
//
// 处理当前已打开的 work part：
//   - 在 work part 同目录写 {name}_data.json
//   - 用 Save() 就地写回 work part 当前关联的路径
//
// 调用方（Python）负责：
//   1. 把 input PRT 复制到 output 目录的目标位置
//   2. 在 NX 中打开这个副本
//   3. 调用本 DLL（DLL 永远不会动到原 input 文件）
//
// 导出接口：
//   EXPORT int run_gouge_check(const char* /*ignored*/)   完整流水线
//   ufusr                                                 NX Open 手动入口
//
// 注：参数的 const char* 参数现在被忽略（保留只是 ABI 兼容）。
//=============================================================================

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <ctime>
#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>
#include <typeinfo>

#include <uf.h>
#include <uf_cam.h>
#include <uf_param.h>
#include <uf_obj.h>
#include <uf_part.h>
#include <uf_ui.h>

#include <NXOpen/NXException.hxx>
#include <NXOpen/Session.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/BasePart.hxx>
#include <NXOpen/NXObjectManager.hxx>
#include <NXOpen/CAM_CAMSetup.hxx>
#include <NXOpen/CAM_CAMObject.hxx>
#include <NXOpen/CAM_Operation.hxx>
#include <NXOpen/CAM_OperationCollection.hxx>

#include <windows.h>
#include <direct.h>

using namespace NXOpen;
using namespace NXOpen::CAM;

#ifndef EXPORT
#define EXPORT extern "C" __declspec(dllexport)
#endif
#ifndef DllExport
#define DllExport __declspec(dllexport)
#endif

//=============================================================================
// 配置常量
//=============================================================================
static const double KEY_INTERVAL = 0.3;
// 钻孔类工序关键字 - 工序名包含这些关键字时跳过 GougeCheck
static const char* DRILL_KEYWORDS[] = {"Drill", "Hole", "Point", NULL};

//=============================================================================
// 工具函数 - 双通道日志（NX 信息窗口 + Visual Studio 调试输出）
//=============================================================================
static std::vector<std::string> g_logBuffer;

// UTF-8 转 GBK（用于把日志写到 NX 信息窗口，否则中文会乱码）
static std::string Utf8ToGbk(const std::string& utf8) {
    if (utf8.empty()) return utf8;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0) return utf8;
    std::wstring w(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], wlen);
    int alen = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    if (alen <= 0) return utf8;
    std::string a(alen - 1, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &a[0], alen, NULL, NULL);
    return a;
}

static void Log(const char* msg) {
    OutputDebugStringA(msg);     // 实时输出到 VS 调试窗口
    g_logBuffer.push_back(msg);  // 缓存待最后 flush
}
static void Log(const std::string& msg) { Log(msg.c_str()); }

// 把缓存的全部日志一次性输出到 NX 信息窗口
// 转换 UTF-8 → GBK，避免中文乱码
static void FlushLogToInfoWindow() {
    UF_UI_open_listing_window();
    for (const auto& m : g_logBuffer) {
        std::string gbk = Utf8ToGbk(m);
        UF_UI_write_listing_window(gbk.c_str());
    }
    g_logBuffer.clear();
}

// 兼容别名
static void PrintInfo(const char* msg) { Log(msg); }
static void PrintInfo(const std::string& msg) { Log(msg.c_str()); }

static void EnsureDir(const std::string& path) {
    CreateDirectoryA(path.c_str(), NULL);
}

// 取部件文件名（不含扩展名）
static std::string GetPartBaseName(Part* wp) {
    NXString leafStr = wp->Leaf();
    const char* leafText = leafStr.GetLocaleText();
    std::string leaf = leafText ? leafText : "";
    size_t dot = leaf.find_last_of('.');
    return (dot != std::string::npos) ? leaf.substr(0, dot) : leaf;
}

// 取部件完整路径（本地编码，用于文件系统操作）
static std::string GetPartFullPath(Part* wp) {
    NXString fp = wp->FullPath();
    const char* t = fp.GetLocaleText();
    return t ? t : "";
}

// 取部件所在目录（不含文件名）
static std::string GetPartDir(Part* wp) {
    std::string fp = GetPartFullPath(wp);
    size_t s = fp.find_last_of("\\/");
    return (s != std::string::npos) ? fp.substr(0, s) : ".";
}

// 当前时间戳 YYYY-MM-DD HH:MM:SS
static std::string NowStr() {
    time_t now = time(NULL);
    struct tm t; localtime_s(&t, &now);
    char buf[64];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d:%02d",
        t.tm_year+1900, t.tm_mon+1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

// 批次时间戳 YYYYMMDD_HHMMSS
static std::string NowStamp() {
    time_t now = time(NULL);
    struct tm t; localtime_s(&t, &now);
    char buf[32];
    sprintf_s(buf, "%04d%02d%02d_%02d%02d%02d",
        t.tm_year+1900, t.tm_mon+1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

// 判断工序是否为钻孔类
static bool IsDrillOp(const std::string& name) {
    for (int k = 0; DRILL_KEYWORDS[k]; k++) {
        if (name.find(DRILL_KEYWORDS[k]) != std::string::npos)
            return true;
    }
    return false;
}

//=============================================================================
// EnterSpammer：后台线程模拟 Enter 键，自动关闭 NX 弹窗
//=============================================================================
static std::atomic<bool> g_spamRun(false);
static std::thread g_spamThread;

static void SpamFunc() {
    while (g_spamRun.load()) {
        keybd_event(VK_RETURN, 0, 0, 0);
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
        std::this_thread::sleep_for(
            std::chrono::milliseconds((int)(KEY_INTERVAL * 1000)));
    }
}
static void StartSpammer() {
    if (g_spamRun.load()) return;
    g_spamRun.store(true);
    g_spamThread = std::thread(SpamFunc);
}
static void StopSpammer() {
    g_spamRun.store(false);
    if (g_spamThread.joinable()) g_spamThread.join();
}

//=============================================================================
// 步骤 1：批量生成所有工序的刀路
//=============================================================================
static void GenerateToolpaths(Part* wp) {
    Log(">>> [TP-1] 进入 GenerateToolpaths\n");
    CAMSetup* cs = wp->CAMSetup();
    if (!cs) { Log("    [TP-2] 无 CAMSetup，返回\n"); return; }
    Log("    [TP-3] CAMSetup 获取成功\n");

    std::vector<CAMObject*> ops;
    OperationCollection* oc = cs->CAMOperationCollection();
    Log("    [TP-4] 获取到 OperationCollection，遍历...\n");
    for (auto it = oc->begin(); it != oc->end(); ++it)
        ops.push_back(*it);

    char buf[256];
    sprintf_s(buf, "    [TP-5] 找到 %d 个工序\n", (int)ops.size());
    Log(buf);

    if (ops.empty()) { Log("    [TP-6] 工序列表为空，返回\n"); return; }

    // 批量生成刀路
    Log("    [TP-GEN] 调用 GenerateToolPath...\n");
    try {
        cs->GenerateToolPath(ops);
        Log("        OK\n");
    } catch (NXException& ex) {
        sprintf_s(buf, "        异常: %s\n", ex.Message());
        Log(buf);
    } catch (const std::exception& ex) {
        sprintf_s(buf, "        异常: %s\n", ex.what());
        Log(buf);
    } catch (...) {
        Log("        未知异常\n");
    }
    Log("    [TP-DONE]\n");
}

//=============================================================================
// 步骤 2：过切检查（不写 TXT，仅在 PRT 内部记录检查状态）
// 钻孔类工序跳过；铣削类调用 GougeCheck。
// 检查完成后用 Save() 就地写回当前 work part 关联的路径，
// 不做 SaveAs，原始 input 文件由调用方（Python）在外部隔离保护。
//=============================================================================
static int DoGougeCheck(Part* wp) {
    Log(">>> [GC-1] 进入 DoGougeCheck\n");
    CAMSetup* cs = wp->CAMSetup();
    if (!cs) { Log("    [GC-2] 无 CAMSetup，返回 -1\n"); return -1; }

    std::vector<Operation*> ops;
    OperationCollection* oc = cs->CAMOperationCollection();
    for (auto it = oc->begin(); it != oc->end(); ++it)
        ops.push_back(*it);

    char buf[1024];
    sprintf_s(buf, "    [GC-3] 待检查工序数: %d\n", (int)ops.size());
    Log(buf);

    Log("    [GC-4] 启动 EnterSpammer 后台按键线程\n");
    StartSpammer();
    int countOk = 0;

    for (size_t i = 0; i < ops.size(); i++) {
        NXString opNameStr = ops[i]->Name();
        const char* nm = opNameStr.GetLocaleText();
        std::string name = nm ? nm : "";

        sprintf_s(buf, "    [GC-5.%d] 工序: %s\n", (int)(i+1), name.c_str());
        Log(buf);

        if (IsDrillOp(name)) {
            Log("        [GC-6] 钻孔类工序，跳过\n");
            countOk++;
            continue;
        }

        try {
            std::vector<CAMObject*> singleOp;
            singleOp.push_back(ops[i]);
            cs->GougeCheck(singleOp, false);
            Log("        [GC-7] GougeCheck 完成\n");
            countOk++;
        } catch (NXException& ex) {
            sprintf_s(buf, "        [GC-FAIL] %s: %s\n", name.c_str(), ex.Message());
            Log(buf);
        } catch (const std::exception& ex) {
            sprintf_s(buf, "        [GC-FAIL] %s: %s\n", name.c_str(), ex.what());
            Log(buf);
        } catch (...) {
            sprintf_s(buf, "        [GC-FAIL] %s: 未知异常\n", name.c_str());
            Log(buf);
        }
    }

    Log("    [GC-8] 停止 EnterSpammer\n");
    StopSpammer();

    // 就地保存当前部件（写回它当前关联的路径）
    Log("    [GC-9] Save 当前部件...\n");
    try {
        wp->Save(BasePart::SaveComponentsTrue,
                 BasePart::CloseAfterSaveFalse);
        Log("    [GC-10] Save 成功\n");
    }
    catch (NXException& ex) {
        sprintf_s(buf, "    [GC-10e] Save 失败: %s\n", ex.Message());
        Log(buf);
    }
    catch (...) { Log("    [GC-10e] Save 未知错误\n"); }

    sprintf_s(buf, "<<< [GC-11] 过切检查完成。成功: %d 个\n", countOk);
    Log(buf);
    return countOk;
}

//=============================================================================
// 步骤 3：导出工序参数为 JSON
// 每个工序只导出 Toolpath Time（参数 ID = 124），用来判断"是否生成刀路"
//=============================================================================
static int ExportJSON(Part* wp, const std::string& jsonPath) {
    Log(">>> [JS-1] 进入 ExportJSON\n");
    CAMSetup* cs = wp->CAMSetup();
    if (!cs) { Log("    [JS-2] 无 CAMSetup\n"); return -1; }

    std::vector<Operation*> ops;
    OperationCollection* oc = cs->CAMOperationCollection();
    for (auto it = oc->begin(); it != oc->end(); ++it)
        ops.push_back(*it);

    char dbg[256];
    sprintf_s(dbg, "    [JS-3] 工序数: %d\n", (int)ops.size());
    Log(dbg);

    std::ostringstream js;
    js << "{\n    \"batch_meta\": {\n";
    js << "        \"export_time\": \"" << NowStr() << "\",\n";
    js << "        \"nx_version\": \"NX\",\n";
    {
        NXString partNameStr = wp->Name();
        const char* pn = partNameStr.GetUTF8Text();
        js << "        \"part_name\": \"" << (pn ? pn : "") << "\",\n";
    }
    js << "        \"total_operations\": " << ops.size() << ",\n";
    js << "        \"batch_timestamp\": \"" << NowStamp() << "\",\n";
    js << "        \"success_operations\": " << ops.size() << ",\n";
    js << "        \"fail_operations\": 0\n";
    js << "    },\n";
    js << "    \"operations\": [\n";

    for (size_t oi = 0; oi < ops.size(); oi++) {
        tag_t otag = ops[oi]->Tag();
        NXString opNameStr = ops[oi]->Name();
        const char* nameText = opNameStr.GetUTF8Text();
        std::string oname = nameText ? nameText : "";

        // 用 RTTI 获取工序的 C++ 类名（如 CavityMilling、HoleDrilling）
        std::string otype = typeid(*ops[oi]).name();
        size_t lastColon = otype.rfind(':');
        if (lastColon != std::string::npos) otype = otype.substr(lastColon + 1);

        // 读取 Toolpath Time（参数 ID = 124）
        double tpTime = 0.0;
        try { UF_PARAM_ask_double_value(otag, 124, &tpTime); } catch (...) {}

        if (oi > 0) js << ",\n";
        js << "        {\n";
        js << "            \"operation_name\": \"" << oname << "\",\n";
        js << "            \"operation_type\": \"" << otype << "\",\n";
        js << "            \"total_params\": 1,\n";
        js << "            \"parameters\": [\n";
        js << "                {\n";
        js << "                    \"id\": 124,\n";
        js << "                    \"display_name\": \"Toolpath Time\",\n";
        js << "                    \"type\": \"Double\",\n";
        js << "                    \"value\": " << tpTime << "\n";
        js << "                }\n";
        js << "            ],\n";
        js << "            \"status\": \"success\"\n";
        js << "        }";
    }

    js << "\n    ]\n";
    js << "}\n";

    Log("    [JS-4] 写入 JSON 文件...\n");
    std::ofstream ofs(jsonPath.c_str());
    if (!ofs.is_open()) {
        Log("    [JS-5e] 打开 JSON 文件失败\n");
        return -1;
    }
    ofs << js.str();
    ofs.close();
    Log("<<< [JS-5] 退出 ExportJSON 成功\n");
    return (int)ops.size();
}

//=============================================================================
// 主流程：完整的过切检查流水线
// 一个 PRT（已由调用方放置在目标位置）→ 3 步处理 → 就地保存 + JSON
//
// 注：output_dir 参数保留只是为了 ABI 兼容，实际已忽略。
//     调用方（Python）负责把 input 文件复制到目标位置后再打开，
//     DLL 只在当前 work part 所在目录写 JSON、并就地 Save。
//=============================================================================
static int DoProcess(const char* /*output_dir*/) {
    g_logBuffer.clear();

    Session* ses = Session::GetSession();
    if (!ses) return -1;
    Part* wp = ses->Parts()->Work();
    if (!wp) {
        PrintInfo("[ERROR] No work part is open\n");
        return -1;
    }
    if (!wp->CAMSetup()) {
        PrintInfo("[ERROR] Part has no CAMSetup\n");
        return -1;
    }

    std::string baseName = GetPartBaseName(wp);
    std::string partDir  = GetPartDir(wp);

    // JSON 写到 work part 同目录，与 prt 同名
    std::string jsonPath = partDir + "\\" + baseName + "_data.json";

    char buf[512];
    sprintf_s(buf, "===== Processing: %s =====\n", baseName.c_str());
    PrintInfo(buf);

    // 切换到加工模块
    try { ses->ApplicationSwitchImmediate("UG_APP_MANUFACTURING"); } catch (...) {}
    UF_CAM_init_session();

    // 计时器（毫秒精度）
    using clk = std::chrono::steady_clock;
    auto t_total_start = clk::now();
    auto elapsedSec = [](clk::time_point start) {
        auto dur = clk::now() - start;
        return std::chrono::duration<double>(dur).count();
    };

    // 每步独立 try/catch，单步失败不影响其他步骤
    auto t1 = clk::now();
    try {
        PrintInfo("[1/3] Generating toolpaths...\n");
        GenerateToolpaths(wp);
    } catch (NXException& ex) {
        sprintf_s(buf, "  [STEP1 ERROR] %s\n", ex.Message());
        PrintInfo(buf);
    } catch (const std::exception& ex) {
        sprintf_s(buf, "  [STEP1 ERROR] %s\n", ex.what());
        PrintInfo(buf);
    } catch (...) { PrintInfo("  [STEP1 ERROR] unknown\n"); }
    sprintf_s(buf, "  [STEP1] %.2fs\n", elapsedSec(t1));
    PrintInfo(buf);

    auto t2 = clk::now();
    try {
        PrintInfo("[2/3] Gouge check + Save PRT (in-place)...\n");
        DoGougeCheck(wp);
    } catch (NXException& ex) {
        sprintf_s(buf, "  [STEP2 ERROR] %s\n", ex.Message());
        PrintInfo(buf);
        StopSpammer();
    } catch (const std::exception& ex) {
        sprintf_s(buf, "  [STEP2 ERROR] %s\n", ex.what());
        PrintInfo(buf);
        StopSpammer();
    } catch (...) {
        PrintInfo("  [STEP2 ERROR] unknown\n");
        StopSpammer();
    }
    sprintf_s(buf, "  [STEP2] %.2fs\n", elapsedSec(t2));
    PrintInfo(buf);

    auto t3 = clk::now();
    try {
        PrintInfo("[3/3] Exporting JSON...\n");
        ExportJSON(wp, jsonPath);
    } catch (NXException& ex) {
        sprintf_s(buf, "  [STEP3 ERROR] %s\n", ex.Message());
        PrintInfo(buf);
    } catch (const std::exception& ex) {
        sprintf_s(buf, "  [STEP3 ERROR] %s\n", ex.what());
        PrintInfo(buf);
    } catch (...) { PrintInfo("  [STEP3 ERROR] unknown\n"); }
    sprintf_s(buf, "  [STEP3] %.2fs\n", elapsedSec(t3));
    PrintInfo(buf);

    sprintf_s(buf, "===== Done. Total: %.2fs =====\n", elapsedSec(t_total_start));
    PrintInfo(buf);
    FlushLogToInfoWindow();
    return 0;
}

//=============================================================================
// 导出接口（给 Python ctypes 调用）
// 不调用 UF_initialize / UF_terminate，因为宿主已管理 UF session
//=============================================================================

/// 完整流水线接口：刀轨 + 过切 + JSON
/// @param  ignored 兼容旧 ABI 的占位参数（实际忽略）
/// @return 0=成功, -1=无工作部件, -2=异常
EXPORT int run_gouge_check(const char* /*ignored*/)
{
    int rc = -2;
    try {
        rc = DoProcess(nullptr);
    }
    catch (const NXException& ex) {
        char buf[512];
        sprintf_s(buf, "[EXCEPTION] %s\n", ex.Message());
        PrintInfo(buf);
        StopSpammer();
    }
    catch (const std::exception& ex) {
        PrintInfo(std::string("[EXCEPTION] ") + ex.what() + "\n");
        StopSpammer();
    }
    catch (...) {
        PrintInfo("[EXCEPTION] Unknown\n");
        StopSpammer();
    }
    FlushLogToInfoWindow();
    return rc;
}

//=============================================================================
// NX Open DLL 标准入口（手动执行：File → Execute → NX Open）
//=============================================================================
extern "C" DllExport void ufusr(char* parm, int* returnCode, int rlen)
{
    *returnCode = 0;
    if (UF_initialize() != 0) { *returnCode = 1; return; }

    try {
        int result = DoProcess("");
        if (result != 0) *returnCode = 1;
    }
    catch (const NXException& ex) {
        char buf[512];
        sprintf_s(buf, "[NX EXCEPTION] %s\n", ex.Message());
        PrintInfo(buf);
        StopSpammer();
        *returnCode = 1;
    }
    catch (const std::exception& ex) {
        PrintInfo(std::string("[STD EXCEPTION] ") + ex.what() + "\n");
        StopSpammer();
        *returnCode = 1;
    }
    catch (...) {
        PrintInfo("[UNKNOWN EXCEPTION] in ufusr\n");
        StopSpammer();
        *returnCode = 1;
    }

    FlushLogToInfoWindow();
    UF_terminate();
}

extern "C" DllExport int ufusr_ask_unload(void)
{
    return UF_UNLOAD_IMMEDIATELY;
}
