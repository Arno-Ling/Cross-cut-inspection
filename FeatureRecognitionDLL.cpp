///* Include files */
#include <stdarg.h>
#include <strstream>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <cmath>

// 添加接口注册所需头文件
#include "dll_interface.h"

using std::ostrstream;
using std::endl;
using std::ends;
using std::cerr;
using std::stringstream;

#include <uf.h>
#include <uf_ui_types.h>
#include <ug_session.hxx>
#include <ug_exception.hxx>
#include <uf_ui.h>
#include <uf_exit.h>
#include <ug_info_window.hxx>

// NX Open头文件
#include <uf_defs.h>
#include <NXOpen/NXException.hxx>
#include <NXOpen/Session.hxx>
#include <NXOpen/Body.hxx>
#include <NXOpen/BodyCollection.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/DisplayableObject.hxx>
#include <NXOpen/NXObject.hxx>
#include <NXOpen/CAM_CAMSetup.hxx>
#include <NXOpen/CAM_CAMObject.hxx>
#include <NXOpen/CAM_FeatureRecognitionBuilder.hxx>
#include <NXOpen/CAM_ManualFeatureBuilder.hxx>
#include <NXOpen/CAM_CAMFeature.hxx>
#include <NXOpen/Face.hxx>
#include <uf_layer.h>
#include <uf_obj.h>

#include <uf_attr.h>
#include <NXOpen/Direction.hxx>
#include <NXOpen/NXObjectManager.hxx>
#include <NXOpen/ScCollector.hxx>
#include <NXOpen/TaggedObject.hxx>
#include <NXOpen/NXObjectManager.hxx>

#include <uf_modl.h>
#include <uf_modl_primitives.h>

using namespace NXOpen;
using namespace NXOpen::CAM;

// 确保 EXPORT 宏
#ifndef EXPORT
#define EXPORT extern "C" __declspec(dllexport)
#endif

static std::ofstream g_logFile;
std::vector<Body*> m_targetBodies;   // 存放工作图层所有实体

// 数学常量定义（替代M_PI）
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -------------- 辅助函数 --------------
static std::string GetFaceGeometryInfo(tag_t faceTag)
{
    int    faceType = 0;
    double point[3] = { 0 };
    double dir[3] = { 0 };
    double box[6] = { 0 };
    double radius = 0;
    double rad_data = 0;
    int    norm_dir = 0;

    if (UF_MODL_ask_face_data(faceTag, &faceType, point, dir, box,
        &radius, &rad_data, &norm_dir) != 0)
        return "";

    double dx = box[3] - box[0];
    double dy = box[4] - box[1];
    double dz = box[5] - box[2];

    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << " L×W×H=" << dx << "×" << dy << "×" << dz;
    if (faceType == UF_MODL_CYLINDRICAL_FACE ||
        faceType == UF_MODL_CONICAL_FACE ||
        faceType == UF_MODL_SPHERICAL_FACE)
        ss << " Diameter≈" << (radius * 2.0);
    return ss.str();
}

std::string GetFaceTypeString(tag_t faceTag)
{
    Face* pFace = dynamic_cast<Face*>(NXObjectManager::Get(faceTag));
    if (!pFace) return "Unknown";

    switch (pFace->SolidFaceType())
    {
    case Face::FaceTypePlanar:      return "Planar";
    case Face::FaceTypeCylindrical: return "Cylindrical";
    case Face::FaceTypeConical:     return "Conical";
    case Face::FaceTypeSpherical:   return "Spherical";
    case Face::FaceTypeBlending:    return "Blending";
    default:                        return "Other Surface";
    }
}

static std::vector<Body*> GetBodiesOnLayer(int targetLayer)
{
    std::vector<Body*> bodies;
    tag_t objectTag = NULL_TAG;

    int layerToCycle = (targetLayer <= 0) ? 0 : targetLayer;
    UF_LAYER_cycle_by_layer(layerToCycle, &objectTag);

    while (objectTag != NULL_TAG)
    {
        int type = 0, subtype = 0;
        if (UF_OBJ_ask_type_and_subtype(objectTag, &type, &subtype) == 0)
        {
            if (type == UF_solid_type)
            {
                TaggedObject* taggedObj = NXObjectManager::Get(objectTag);
                if (taggedObj != nullptr)
                {
                    Body* pBody = dynamic_cast<Body*>(taggedObj);
                    if (pBody != nullptr)
                    {
                        bodies.push_back(pBody);
                    }
                }
            }
        }
        UF_LAYER_cycle_by_layer(layerToCycle, &objectTag);
    }

    return bodies;
}

static std::string GetFaceAttributeTag(tag_t faceTag)
{
    UF_initialize();
    std::string ret;

    int title_type = 0;
    char title[] = "FACE_TAG";
    if (UF_ATTR_find_attribute(faceTag, UF_ATTR_any, title, &title_type) == 0 &&
        title_type == UF_ATTR_string)
    {
        UF_ATTR_value_t value;
        if (UF_ATTR_read_value(faceTag, title, UF_ATTR_any, &value) == 0 &&
            value.value.string != nullptr)
        {
            ret = value.value.string;
            UF_free(value.value.string);
        }
    }
    UF_terminate();
    return ret;
}

static int GetFaceColorIndex(tag_t faceTag)
{
    UF_OBJ_disp_props_t disp_props;
    if (UF_OBJ_ask_display_properties(faceTag, &disp_props) == 0)
        return disp_props.color;
    return -1;
}

double GetFaceAngleWithZ(tag_t faceTag)
{
    double uvMinMax[4] = { 0 };
    UF_MODL_ask_face_uv_minmax(faceTag, uvMinMax);
    double uvMid[2] = {
        (uvMinMax[0] + uvMinMax[1]) * 0.5,
        (uvMinMax[2] + uvMinMax[3]) * 0.5
    };

    double point[3], u1[3], v1[3], u2[3], v2[3], normal[3], radii[2];
    if (UF_MODL_ask_face_props(faceTag, uvMid, point, u1, v1, u2, v2, normal, radii) != 0)
        return 91.9999;

    double cosVal = std::fabs(normal[2]);
    return std::acos(cosVal) * 180.0 / M_PI;
}

bool IsPlanarBottomFace(NXOpen::Face* face)
{
    if (!face) return false;

    tag_t faceTag = face->Tag();
    int type = 0;
    if (UF_MODL_ask_face_type(faceTag, &type) != 0)
        return false;
    if (type != UF_MODL_PLANAR_FACE)
        return false;

    double uvMinMax[4] = { 0 };
    UF_MODL_ask_face_uv_minmax(faceTag, uvMinMax);
    double uvMid[2] = {
        (uvMinMax[0] + uvMinMax[1]) * 0.5,
        (uvMinMax[2] + uvMinMax[3]) * 0.5
    };

    double point[3], u1[3], v1[3], u2[3], v2[3], normal[3], radii[2];
    if (UF_MODL_ask_face_props(faceTag, uvMid, point, u1, v1, u2, v2, normal, radii) != 0)
        return false;

    const double cosLimit = std::cos(2.0 * M_PI / 180.0);
    return std::fabs(normal[2]) > cosLimit;
}

void print_to_info_window(const char* message) {
    UF_UI_open_listing_window();
    UF_UI_write_listing_window(message);
}

void print_to_info_window(const std::string& message) {
    print_to_info_window(message.c_str());
}

void print_to_info_window_and_file(const std::string& message) {
    print_to_info_window(message.c_str());
    if (g_logFile.is_open()) {
        g_logFile << message;
        g_logFile.flush();
    }
}

/*****************************************************************************
** FeatureRecognition类定义
*****************************************************************************/
class FeatureRecognition
{
public:
    FeatureRecognition();
    virtual ~FeatureRecognition();

    bool ExecuteFeatureRecognition();
    void SetTargetBody(Body* body);
    void SetTargetBodyByName(const char* bodyName);
    void SetUseFeatureNameAsType(bool useName) { m_useFeatureNameAsType = useName; }
    void SetMapFeatures(bool mapFeatures) { m_mapFeatures = mapFeatures; }
    void SetAssignColor(bool assignColor) { m_assignColor = assignColor; }
    void SetAddCadFeatureAttributes(bool addAttributes) { m_addCadFeatureAttributes = addAttributes; }
    void SetIgnoreWarnings(bool ignoreWarnings) { m_ignoreWarnings = ignoreWarnings; }
    void ProcessRecognizedFeatures(const std::vector<CAMFeature*>& features);
    void SetTargetLayer(int layer) { m_targetLayer = layer; }
    int  GetTargetLayer() const { return m_targetLayer; }
    const std::vector<CAMFeature*>& GetRecognizedFeatures() const {
        return m_recognizedFeatures;
    }
    void GenerateCSVReport(const std::string& csvPath);

private:
    Session* m_session;
    Part* m_workPart;
    Body* m_targetBody;

    bool m_useFeatureNameAsType;
    bool m_mapFeatures;
    bool m_assignColor;
    bool m_addCadFeatureAttributes;
    bool m_ignoreWarnings;

    int m_targetLayer = 21;
    std::vector<std::string> m_featureTypes;
    std::vector<CAMFeature*> m_recognizedFeatures;

    void InitializeFeatureTypes();
    bool ValidateEnvironment();
    bool PerformRecognition();

    struct FaceGeometryInfo {
        double length = 0.0;
        double width = 0.0;
        double height = 0.0;
        double diameter = 0.0;
        double angle = 0.0;
    };

    FaceGeometryInfo ParseFaceGeometryInfo(tag_t faceTag);
};

/*****************************************************************************
** FeatureRecognition类实现
*****************************************************************************/
FeatureRecognition::FeatureRecognition()
    : m_session(nullptr)
    , m_workPart(nullptr)
    , m_targetBody(nullptr)
    , m_useFeatureNameAsType(true)
    , m_mapFeatures(true)
    , m_assignColor(false)
    , m_addCadFeatureAttributes(false)
    , m_ignoreWarnings(false)
{
    m_session = Session::GetSession();
    if (m_session != nullptr)
    {
        m_workPart = m_session->Parts()->Work();
    }
    InitializeFeatureTypes();
}

FeatureRecognition::~FeatureRecognition()
{
}

void FeatureRecognition::ProcessRecognizedFeatures(const std::vector<CAMFeature*>& features)
{
    m_recognizedFeatures = features;

    stringstream ss;
    ss << "Recognized " << features.size() << " machining features\n";
    ss << "========================================\n";
    print_to_info_window_and_file(ss.str());

    for (size_t i = 0; i < features.size(); ++i)
    {
        CAMFeature* feature = features[i];
        stringstream featureInfo;

        featureInfo << "Feature " << (i + 1) << " Type: " << feature->Type().GetText() << "\n";

        std::vector<Face*> featureFaces;
        try {
            featureFaces = feature->GetFaces();
        }
        catch (const NXException&) {
            try {
                for (auto* obj : feature->GetGeometry())
                    if (auto* f = dynamic_cast<Face*>(obj)) featureFaces.push_back(f);
            }
            catch (...) { /* ignore */ }
        }

        if (!featureFaces.empty())
        {
            featureInfo << "Face tags: ";
            for (Face* face : featureFaces)
            {
                tag_t t = face->Tag();
                std::string attr = GetFaceAttributeTag(t);
                if (attr.empty()) featureInfo << t << " ";
                else              featureInfo << t << "(FACE_TAG: " << attr << ") ";
            }
            featureInfo << "\n";
        }
        else
        {
            featureInfo << "This feature has no corresponding faces\n";
        }

        std::string featType = feature->Type().GetText();
        bool isPocket = (featType.find("POCKET") != std::string::npos);
        bool singleFace = (featureFaces.size() == 1);

        if (isPocket) featureInfo << "【POCKET Feature Processing Start】\n";

        for (Face* face : featureFaces)
        {
            tag_t tag = face->Tag();

            if (!singleFace)
            {
                std::string attr = GetFaceAttributeTag(tag);
                featureInfo << "  Face Tag = " << tag;
                if (!attr.empty()) featureInfo << "(FACE_TAG: " << attr << ")";
                featureInfo << " ";
            }
            else
            {
                featureInfo << "  ";
            }

            int colorIdx = GetFaceColorIndex(tag);
            std::string faceTypeStr = GetFaceTypeString(tag);
            featureInfo << "(" << faceTypeStr << ")"
                << "(Color Index " << colorIdx << ")"
                << GetFaceGeometryInfo(tag);

            if (isPocket)
            {
                if (IsPlanarBottomFace(face))
                    featureInfo << " → Bottom Face";
                else
                {
                    double angle = GetFaceAngleWithZ(tag);
                    featureInfo << "  " << (angle < 44.99 ? "Gentle Face" : "Steep Face") << " " << angle << "°"
                        << " → Side Wall";
                }
            }
            featureInfo << "\n";
        }

        if (isPocket) featureInfo << "【POCKET Feature Processing End】\n";
        featureInfo << "----------------------------------------\n";
        print_to_info_window_and_file(featureInfo.str());
    }
}

FeatureRecognition::FaceGeometryInfo FeatureRecognition::ParseFaceGeometryInfo(tag_t faceTag)
{
    FaceGeometryInfo info;

    int faceType = 0;
    double point[3] = { 0 };
    double dir[3] = { 0 };
    double box[6] = { 0 };
    double radius = 0;
    double rad_data = 0;
    int norm_dir = 0;

    if (UF_MODL_ask_face_data(faceTag, &faceType, point, dir, box,
        &radius, &rad_data, &norm_dir) == 0)
    {
        info.length = box[3] - box[0];
        info.width = box[4] - box[1];
        info.height = box[5] - box[2];
        info.diameter = (faceType == UF_MODL_CYLINDRICAL_FACE ||
            faceType == UF_MODL_CONICAL_FACE ||
            faceType == UF_MODL_SPHERICAL_FACE) ? radius * 2.0 : 0.0;
    }

    info.angle = GetFaceAngleWithZ(faceTag);

    return info;
}

void FeatureRecognition::GenerateCSVReport(const std::string& csvPath)
{
    std::ofstream csvFile(csvPath);
    if (!csvFile.is_open())
    {
        print_to_info_window_and_file("❌ Unable to create CSV file: " + csvPath + "\n");
        return;
    }

    // 修改列标题为英文
    csvFile << "ID,Type,FaceTag,Attribute,FaceType,Color,Length,Width,Height,Dia,Angle,Role,SideType,IsBottom,IsSide\n";

    int featureIndex = 1;
    for (const auto& feature : m_recognizedFeatures)
    {
        std::string featureType = feature->Type().GetText();
        std::vector<Face*> featureFaces;

        try {
            featureFaces = feature->GetFaces();
        }
        catch (const NXException&) {
            try {
                for (auto* obj : feature->GetGeometry())
                    if (auto* f = dynamic_cast<Face*>(obj))
                        featureFaces.push_back(f);
            }
            catch (...) { /* ignore */ }
        }

        bool isPocket = (featureType.find("POCKET") != std::string::npos);

        for (Face* face : featureFaces)
        {
            tag_t tag = face->Tag();
            std::string faceAttr = GetFaceAttributeTag(tag);
            std::string faceTypeStr = GetFaceTypeString(tag);
            int colorIdx = GetFaceColorIndex(tag);

            auto geomInfo = ParseFaceGeometryInfo(tag);

            std::string role = "";
            std::string sideType = "";
            bool isBottom = false;
            bool isSide = false;

            if (isPocket)
            {
                if (IsPlanarBottomFace(face))
                {
                    role = "Bottom";
                    isBottom = true;
                }
                else
                {
                    role = "Wall";
                    isSide = true;
                    double angle = GetFaceAngleWithZ(tag);
                    sideType = (angle < 44.99) ? "Gentle" : "Steep";
                }
            }

            // 确保所有值都是英文的
            csvFile << featureIndex << ","
                << featureType << ","
                << tag << ","
                << faceAttr << ","
                << faceTypeStr << ","
                << colorIdx << ","
                << geomInfo.length << ","
                << geomInfo.width << ","
                << geomInfo.height << ","
                << geomInfo.diameter << ","
                << geomInfo.angle << ","
                << role << ","
                << sideType << ","
                << (isBottom ? "Yes" : "No") << ","
                << (isSide ? "Yes" : "No") << "\n";
        }

        featureIndex++;
    }

    csvFile.close();
    print_to_info_window_and_file("✅ CSV report generated: " + csvPath + "\n");
}

void FeatureRecognition::InitializeFeatureTypes()
{
    const char* types[] = {
         "CORNER_NOTCH_STRAIGHT", "SLOT_PARTIAL_RECTANGULAR",
        "SLOT_PARTIAL_U_SHAPED", "STEP2POCKET_THREAD",

        "POCKET_ROUND_TAPERED", "STEP2POCKET",
        "POCKET_RECTANGULAR_STRAIGHT",
        "STEP2POCKET_THREAD",
         "STEP2POCKET",  "STEP2POCKET_THREAD",
        "BOSS_RECTANGULAR_STRAIGHT",
        "BOSS_ROUND_STRAIGHT", "BOSS_ROUND_STRAIGHT_THREAD", "CORNER_NOTCH_RECTANGULAR",
        "CORNER_NOTCH_ROUND_CONCAVE", "CORNER_NOTCH_STRAIGHT", "CORNER_NOTCH_U_SHAPED",
        "GROOVE_AX_CIRCULAR_RECT", "GROOVE_INS_RAD_RECT", "POCKET_ROUND_TAPERED",
        "POCKET_CLOSED", "POCKET_OBROUND_CURVED_STRAIGHT", "POCKET_FREE_SHAPED_STRAIGHT",
        "POCKET_OPEN", "POCKET_OBROUND_STRAIGHT", "POCKET_RECTANGULAR_STRAIGHT",
        "SIDE_NOTCH_RECTANGULAR", "SIDE_NOTCH_ROUND_CONCAVE", "SIDE_NOTCH_U_SHAPED",
        "SLOT_90_DEGREE", "SLOT_DOVE_TAIL", "SLOT_OBROUND", "SLOT_PARTIAL_OBROUND",
        "SLOT_PARTIAL_RECTANGULAR", "SLOT_PARTIAL_ROUND", "SLOT_PARTIAL_U_SHAPED",
        "SLOT_RECTANGULAR", "SLOT_ROUND", "SLOT_T_SHAPED", "SLOT_U_SHAPED",
        "SLOT_UPSIDE_DOWN_DOVE_TAIL", "SLOT_V_SHAPED", "STEP2POCKET",
         "STEP3POCKET", "STEP4POCKET",
         "STEP5POCKET", "STEP6POCKET",
         "STEP2POCKET_THREAD","STEP3POCKET_THREAD",
         "STEP4POCKET_THREAD", "STEP5POCKET_THREAD", "STEP6POCKET_THREAD", "SURFACE_PLANAR",
        "SURFACE_PLANAR_RECTANGULAR", "SURFACE_PLANAR_ROUND", "TURNING_GROOVE_FACE",
        "TURNING_GROOVE_ID", "TURNING_GROOVE_OD", /*"WEDM_FREE_SHAPED_STRAIGHT",*/
        /*"WEDM_OBROUND_STRAIGHT", "WEDM_RECTANGULAR_STRAIGHT", "WEDM_ROUND_STRAIGHT"*/
    };

    m_featureTypes.clear();
    int typeCount = sizeof(types) / sizeof(types[0]);
    for (int i = 0; i < typeCount; i++)
    {
        m_featureTypes.push_back(types[i]);
    }
}

bool FeatureRecognition::ValidateEnvironment()
{
    if (!m_session || !m_workPart)
        return false;

    m_targetBodies.clear();
    m_targetBodies = GetBodiesOnLayer(m_targetLayer);

    if (m_targetBodies.empty())
    {
        char buf[256];
        sprintf_s(buf, sizeof(buf), "Error: No solid bodies found on layer %d (or bodies are hidden)!\n", m_targetLayer);
        print_to_info_window_and_file(buf);
        return false;
    }

    char buf[256];
    sprintf_s(buf, sizeof(buf), "Successfully obtained %d bodies on layer %d, starting feature recognition...\n",
        m_targetLayer, (int)m_targetBodies.size());
    print_to_info_window_and_file(buf);

    return true;
}

void FeatureRecognition::SetTargetBody(Body* body)
{
    m_targetBody = body;
}

void FeatureRecognition::SetTargetBodyByName(const char* bodyName)
{
    if (m_workPart != nullptr)
    {
        BodyCollection* bodyCollection = m_workPart->Bodies();
        if (bodyCollection != nullptr)
        {
            m_targetBody = dynamic_cast<Body*>(bodyCollection->FindObject(bodyName));
        }
    }
}

bool FeatureRecognition::ExecuteFeatureRecognition()
{
    if (!ValidateEnvironment())
    {
        return false;
    }

    Session::UndoMarkId markId1 = m_session->SetUndoMark(
        Session::MarkVisibilityInvisible, "Start Feature Recognition");

    Session::UndoMarkId markId2 = m_session->SetUndoMark(
        Session::MarkVisibilityVisible, "Feature Recognition");

    bool result = false;
    try
    {
        result = PerformRecognition();

        if (result)
        {
            m_session->SetUndoMarkName(markId2, "Find Features");
        }

        m_session->DeleteUndoMark(markId1, NULL);
    }
    catch (const NXException& ex)
    {
        m_session->DeleteUndoMark(markId1, NULL);
        result = false;
    }

    return result;
}

bool FeatureRecognition::PerformRecognition()
{
    CAMSetup* camSetup = m_workPart->CAMSetup();
    if (camSetup == nullptr)
    {
        return false;
    }

    CAMObject* nullCAMObject = NULL;
    FeatureRecognitionBuilder* featureBuilder = camSetup->CreateFeatureRecognitionBuilder(nullCAMObject);
    if (featureBuilder == nullptr)
    {
        return false;
    }

    ManualFeatureBuilder* manualBuilder = featureBuilder->CreateManualFeatureBuilder();
    if (manualBuilder == nullptr)
    {
        featureBuilder->Destroy();
        return false;
    }

    bool success = false;

    try
    {
        featureBuilder->SetAssignColor(m_assignColor);
        featureBuilder->SetAddCadFeatureAttributes(m_addCadFeatureAttributes);
        featureBuilder->SetMapFeatures(m_mapFeatures);
        featureBuilder->SetUseFeatureNameAsType(m_useFeatureNameAsType);
        featureBuilder->SetIgnoreWarnings(m_ignoreWarnings);

        featureBuilder->SetRecognitionType(FeatureRecognitionBuilder::RecognitionEnumParametric);
        featureBuilder->SetGeometrySearchType(FeatureRecognitionBuilder::GeometrySearchSelected);

        std::vector<Direction*> machiningDirections(0);
        featureBuilder->SetMachiningAccessDirection(machiningDirections, 0.0);

        std::vector<NXString> nxFeatureTypes;
        for (const auto& type : m_featureTypes)
        {
            nxFeatureTypes.push_back(NXString(type.c_str()));
        }
        featureBuilder->SetFeatureTypes(nxFeatureTypes);

        std::vector<DisplayableObject*> searchObjects;
        searchObjects.reserve(m_targetBodies.size());
        for (auto* body : m_targetBodies)
            searchObjects.push_back(body);
        featureBuilder->SetSearchGeometry(searchObjects);

        std::vector<CAMFeature*> features = featureBuilder->FindFeatures();
        print_to_info_window_and_file("Recognized feature list:\n");

        ProcessRecognizedFeatures(features);

        Session::UndoMarkId markId3 = m_session->SetUndoMark(
            Session::MarkVisibilityInvisible, "Find Features Commit");

        /*NXObject* result = featureBuilder->Commit();*/
        /*success = (result != nullptr);*/

        m_session->DeleteUndoMark(markId3, NULL);
    }
    catch (const NXException& ex)
    {
        success = false;
    }

    featureBuilder->Destroy();
    manualBuilder->Destroy();

    return success;
}

// ===========================================================================
// 导出接口函数
// ===========================================================================

// 辅助函数：智能字符串转换
static std::string ToStdString(const NXOpen::NXString& nxStr) {
    return std::string(nxStr.GetLocaleText());
}
static std::string ToStdString(const char* str) {
    return std::string(str ? str : "");
}
static std::string ToStdString(const std::string& str) {
    return str;
}

// 导出接口函数：运行特征识别
EXPORT int run_feature_recognition(int target_layer, const char* output_dir)
{
    try
    {
        UF_initialize();

        Session* theSession = Session::GetSession();
        Part* workPart = theSession->Parts()->Work();
        if (!workPart) {
            return -1; // No working part
        }

        // 确定日志文件路径
        std::string logPath;
        if (output_dir && output_dir[0] != '\0') {
            // 使用指定的输出目录
            std::string partName = workPart->Leaf().GetText();
            std::string baseName = partName.substr(0, partName.find_last_of('.'));
            logPath = std::string(output_dir) + "\\" + baseName + "_FeatureRecognition_Log.txt";
        }
        else {
            // 使用默认路径
            std::string fullPath = workPart->FullPath().GetText();
            size_t dot = fullPath.rfind('.');
            logPath = (dot == std::string::npos ? fullPath : fullPath.substr(0, dot))
                + "_FeatureRecognition_Log.txt";
        }

        // 打开日志文件
        g_logFile.open(logPath, std::ios::out | std::ios::app);
        if (!g_logFile.is_open()) {
            print_to_info_window_and_file("❌ Unable to create log file, output only to info window.\n");
        }
        else {
            print_to_info_window_and_file("✅ Log file created: " + logPath + "\n");
        }

        // 执行特征识别
        FeatureRecognition featureRecog;
        featureRecog.SetTargetLayer(target_layer);
        featureRecog.SetAssignColor(false);
        featureRecog.SetAddCadFeatureAttributes(false);
        featureRecog.SetMapFeatures(true);
        featureRecog.SetUseFeatureNameAsType(true);
        featureRecog.SetIgnoreWarnings(false);

        bool result = featureRecog.ExecuteFeatureRecognition();

        // 生成CSV报告
        std::string csvPath = logPath.substr(0, logPath.find_last_of('.')) + ".csv";
        featureRecog.GenerateCSVReport(csvPath);

        // 获取识别的特征数量
        int recognizedCount = featureRecog.GetRecognizedFeatures().size();

        // 关闭日志文件
        if (g_logFile.is_open()) {
            g_logFile.close();
        }

        // 修正：根据实际识别到的特征数量判断成功与否
        if (recognizedCount > 0) {
            print_to_info_window_and_file("✅ Feature recognition completed! Total " + std::to_string(recognizedCount) + " features recognized.\n");
            return 0; // Success
        }
        else {
            print_to_info_window_and_file("⚠️ No features recognized!\n");
            return -2; // No features recognized
        }
    }
    catch (const std::exception& e)
    {
        print_to_info_window_and_file("❌ Exception occurred during feature recognition: " + std::string(e.what()) + "\n");
        return -3;
    }
    catch (...)
    {
        print_to_info_window_and_file("❌ Unknown exception occurred during feature recognition\n");
        return -4;
    }
}

// ===========================================================================
// 原有的ufusr函数（保持原有功能不变）
// ===========================================================================

static void processException(const UgException& exception);

extern "C" DllExport void ufusr(char* parm, int* returnCode, int rlen)
{
    *returnCode = 0;
    UgSession session(true);

    std::string logPath;
    try
    {
        Session* theSession = Session::GetSession();
        Part* workPart = theSession->Parts()->Work();
        if (!workPart) throw std::runtime_error("No part is opened");

        std::string fullPath = workPart->FullPath().GetText();
        size_t dot = fullPath.rfind('.');
        logPath = (dot == std::string::npos ? fullPath
            : fullPath.substr(0, dot))
            + "_FeatureRecognition_Log.txt";
    }
    catch (...)
    {
        logPath = std::string(std::getenv("USERPROFILE"))
            + "\\Desktop\\NX_FeatureRecognition_Log.txt";
    }

    g_logFile.open(logPath, std::ios::out | std::ios::app);
    if (!g_logFile.is_open())
        print_to_info_window_and_file("❌ Unable to create log file, output only to info window.\n");
    else
        print_to_info_window_and_file("✅ Log file created: " + logPath + "\n");

    try
    {
        FeatureRecognition featureRecog;
        featureRecog.SetTargetLayer(20);
        featureRecog.SetAssignColor(false);
        featureRecog.SetAddCadFeatureAttributes(false);
        featureRecog.SetMapFeatures(true);
        featureRecog.SetUseFeatureNameAsType(true);
        featureRecog.SetIgnoreWarnings(false);

        bool result = featureRecog.ExecuteFeatureRecognition();

        std::string csvPath = logPath.substr(0, logPath.find_last_of('.')) + ".csv";
        featureRecog.GenerateCSVReport(csvPath);

        // 修正：根据特征数量判断，而不是 result 返回值
        int featureCount = featureRecog.GetRecognizedFeatures().size();
        if (featureCount > 0)
        {
            print_to_info_window_and_file("✅ Feature recognition completed! Total " + std::to_string(featureCount) + " features recognized.\n");
            *returnCode = 0; // 成功
        }
        else
        {
            *returnCode = 1;
            print_to_info_window_and_file("⚠️ No features recognized!\n");
        }
    }
    catch (...) { *returnCode = 1; }

    if (g_logFile.is_open()) g_logFile.close();
}

extern "C" int ufusr_ask_unload(void)
{
    return UF_UNLOAD_UG_TERMINATE;
}

static void processException(const UgException& exception)
{
    ostrstream error_message;

    error_message << endl
        << "Error:" << endl
        << (exception.askErrorText()).c_str()
        << endl << endl << ends;

    UgInfoWindow::open();
    UgInfoWindow::write(error_message.str());
    cerr << error_message.str();
    error_message.rdbuf()->freeze(0);
}

// ===========================================================================
// [注册接口]
// ===========================================================================

REG_START()
REG_FUNC(run_feature_recognition, TYPE_INT)
PARAM(target_layer, TYPE_INT)
PARAM(output_dir, TYPE_STRING)
END_FUNC()
REG_END()

ENABLE_REFLECTION()