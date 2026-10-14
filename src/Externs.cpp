#include "Externs.hpp"

#include <algorithm>
#include <array>

namespace UdonQuic
{
    bool ParseSignature(std::string_view text, Signature& out)
    {
        size_t dot = text.find(".__");
        if (dot == std::string_view::npos) return false;
        out = {};
        out.module = std::string(text.substr(0, dot));
        std::string_view rest = text.substr(dot + 3);

        size_t end = rest.find("__");
        if (end == std::string_view::npos) return false;
        out.method = std::string(rest.substr(0, end));
        rest = rest.substr(end + 2);

        size_t split = rest.find("__");
        if (split == std::string_view::npos)
        {
            out.result = std::string(rest);
            return !out.result.empty();
        }

        std::string_view params = rest.substr(0, split);
        out.result = std::string(rest.substr(split + 2));
        while (!params.empty())
        {
            size_t u = params.find('_');
            out.parameters.emplace_back(params.substr(0, u));
            if (u == std::string_view::npos) break;
            params = params.substr(u + 1);
        }
        return !out.result.empty();
    }

    namespace
    {
        constexpr std::array Primitives = {
            "SystemBoolean", "SystemByte", "SystemSByte", "SystemInt16", "SystemUInt16", "SystemInt32",
            "SystemUInt32", "SystemInt64", "SystemUInt64", "SystemSingle", "SystemDouble", "SystemChar",
        };

        constexpr std::array Vectors = {
            "UnityEngineVector2", "UnityEngineVector3", "UnityEngineVector4", "UnityEngineVector2Int",
            "UnityEngineVector3Int", "UnityEngineQuaternion", "UnityEngineColor", "UnityEngineColor32", "UnityEngineMathf",
        };

        constexpr std::array Integers = {
            "SystemByte", "SystemSByte", "SystemInt16", "SystemUInt16", "SystemInt32", "SystemUInt32", "SystemInt64", "SystemUInt64", "SystemChar",
        };

        template <size_t N>
        bool In(const std::array<const char*, N>& set, std::string_view s)
        {
            return std::any_of(set.begin(), set.end(), [&](const char* x) { return s == x; });
        }

        bool ValueResult(std::string_view type)
        {
            return In(Primitives, type) || In(Vectors, type) || type == "SystemString";
        }
    }

    namespace
    {
        constexpr std::array ReadExact = {
            "VRCSDKBaseUtilities.__IsValid__SystemObject__SystemBoolean",
            "UnityEngineObject.__op_Equality__UnityEngineObject_UnityEngineObject__SystemBoolean",
            "UnityEngineObject.__op_Inequality__UnityEngineObject_UnityEngineObject__SystemBoolean",
            "UnityEngineObject.__op_Implicit__UnityEngineObject__SystemBoolean",
            "SystemObject.__op_Equality__SystemObject_SystemObject__SystemBoolean",
            "SystemObject.__op_Inequality__SystemObject_SystemObject__SystemBoolean",
            "VRCSDKBaseVRCPlayerApi.__get_isLocal__SystemBoolean",
            "VRCSDKBaseVRCPlayerApi.__get_playerId__SystemInt32",
            "VRCSDKBaseVRCPlayerApi.__get_displayName__SystemString",
            "VRCSDKBaseVRCPlayerApi.__get_isMaster__SystemBoolean",
            "VRCSDKBaseVRCPlayerApi.__GetPosition__UnityEngineVector3",
            "VRCSDKBaseVRCPlayerApi.__GetRotation__UnityEngineQuaternion",
            "VRCSDKBaseVRCPlayerApi.__IsUserInVR__SystemBoolean",
            "VRCSDKBaseVRCPlayerApi.__IsValid__SystemBoolean",
            "VRCSDKBaseNetworking.__get_LocalPlayer__VRCSDKBaseVRCPlayerApi",
            "VRCSDKBaseNetworking.__get_IsMaster__SystemBoolean",
            "VRCSDKBaseNetworking.__IsOwner__UnityEngineGameObject__SystemBoolean",
            "VRCSDKBaseNetworking.__IsOwner__VRCSDKBaseVRCPlayerApi_UnityEngineGameObject__SystemBoolean",
            "VRCSDKBaseNetworking.__GetOwner__UnityEngineGameObject__VRCSDKBaseVRCPlayerApi",
            "UnityEngineTime.__get_time__SystemSingle",
            "UnityEngineTime.__get_deltaTime__SystemSingle",
            "UnityEngineTime.__get_fixedDeltaTime__SystemSingle",
            "UnityEngineTime.__get_frameCount__SystemInt32",
            "UnityEngineTime.__get_timeSinceLevelLoad__SystemSingle",
            "UnityEngineTime.__get_unscaledDeltaTime__SystemSingle",
            "UnityEngineComponent.__get_transform__UnityEngineTransform",
            "UnityEngineComponent.__get_gameObject__UnityEngineGameObject",
            "UnityEngineGameObject.__get_transform__UnityEngineTransform",
            "UnityEngineGameObject.__get_activeSelf__SystemBoolean",
            "UnityEngineGameObject.__get_activeInHierarchy__SystemBoolean",
            "UnityEngineBehaviour.__get_enabled__SystemBoolean",
            "UnityEngineTransform.__get_position__UnityEngineVector3",
            "UnityEngineTransform.__get_rotation__UnityEngineQuaternion",
            "UnityEngineTransform.__get_localPosition__UnityEngineVector3",
            "UnityEngineTransform.__get_localRotation__UnityEngineQuaternion",
            "UnityEngineTransform.__get_localScale__UnityEngineVector3",
            "UnityEngineTransform.__get_forward__UnityEngineVector3",
            "UnityEngineTransform.__get_right__UnityEngineVector3",
            "UnityEngineTransform.__get_up__UnityEngineVector3",
            "UnityEngineTransform.__get_parent__UnityEngineTransform",
            "UnityEngineTransform.__get_childCount__SystemInt32",
        };

        constexpr std::array WriteModules = {
            "UnityEngineTransform", "TMProTextMeshProUGUI", "TMProTextMeshPro", "TMProTMP_Text", "UnityEngineDebug", "UnityEngineRigidbody",
            "UnityEngineMaterial", "UnityEngineAnimator", "UnityEngineAudioSource", "UnityEngineLineRenderer", "UnityEngineTrailRenderer",
            "UnityEngineUIImage", "UnityEngineUIText", "UnityEngineUIGraphic", "UnityEngineMeshRenderer", "UnityEngineSkinnedMeshRenderer",
            "UnityEngineRenderer", "UnityEngineLight", "UnityEngineParticleSystem", "UnityEngineMaterialPropertyBlock",
        };

        constexpr std::array WriteExcluded = { "SetParent", "set_parent", "DetachChildren", "SetAsFirstSibling", "SetAsLastSibling", "SetSiblingIndex",
                                               "Destroy", "set_enabled", "set_gameObject", "Update", "Rebind", "WriteDefaultValues", "Simulate",
                                               "SendMessage", "BroadcastMessage", "SendMessageUpwards" };

        bool IsWrite(const Signature& sig)
        {
            if (sig.module.ends_with("Array") && sig.method == "Set" && sig.result == "SystemVoid") return true;
            if (std::none_of(WriteModules.begin(), WriteModules.end(), [&](const char* m) { return sig.module == m; })) return false;
            if (std::any_of(WriteExcluded.begin(), WriteExcluded.end(), [&](const char* m) { return sig.method == m; })) return false;
            if (sig.method.starts_with("get_") || sig.method.starts_with("Get")) return false;
            return sig.result == "SystemVoid";
        }

        bool IsArrayGet(const Signature& sig)
        {
            return sig.module.ends_with("Array") && sig.method == "Get" && sig.parameters.size() == 1 && sig.parameters[0] == "SystemInt32" &&
                   !sig.result.ends_with("Array");
        }
    }

    Effect Classify(std::string_view text)
    {
        Signature sig;
        if (!ParseSignature(text, sig)) return Effect::Barrier;
        if (text == "SystemArray.__get_Length__SystemInt32") return Effect::Pure;
        if (std::any_of(ReadExact.begin(), ReadExact.end(), [&](const char* s) { return text == s; }) || IsArrayGet(sig)) return Effect::Read;
        if (IsWrite(sig)) return Effect::Write;

        bool primitive = In(Primitives, sig.module);
        bool vector = In(Vectors, sig.module);
        bool other = sig.module == "SystemMath" || sig.module == "SystemConvert" || sig.module == "SystemString";
        if (!primitive && !vector && !other) return Effect::Barrier;

        if (!ValueResult(sig.result)) return Effect::Barrier;
        if (sig.method == "ctor" && vector && sig.result == sig.module &&
            std::all_of(sig.parameters.begin(), sig.parameters.end(), [](const std::string& p) { return p == "SystemSingle" || p == "SystemInt32"; }))
            return Effect::PureNoThrow;
        if (sig.method.starts_with("set_") || sig.method == "ctor") return Effect::Barrier;
        for (const std::string& p : sig.parameters)
            if (p.ends_with("Ref") || p.ends_with("Array") || (!ValueResult(p) && p != "SystemObject" && p != "SystemIFormatProvider"))
                return Effect::Barrier;
        if (std::any_of(sig.parameters.begin(), sig.parameters.end(), [](const std::string& p) { return p == "SystemObject" || p == "SystemIFormatProvider"; }))
            return sig.module == "SystemString" ? Effect::Barrier : Effect::Pure;

        if (sig.module == "SystemString" && sig.method != "op_Equality" && sig.method != "op_Inequality" && sig.method != "Concat" &&
            sig.method != "get_Length" && sig.method != "Equals")
            return Effect::Barrier;

        if (other || sig.method == "op_Division" || sig.method == "op_Remainder") return Effect::Pure;
        if (sig.method == "get_Item" || sig.method == "Parse" || sig.method.starts_with("To")) return Effect::Pure;
        if (primitive && In(Integers, sig.module) && (sig.method == "op_Division" || sig.method == "op_Remainder")) return Effect::Pure;
        if (primitive && !sig.method.starts_with("op_") && sig.method != "Equals" && sig.method != "CompareTo" && sig.method != "GetHashCode")
            return Effect::Pure;
        return Effect::PureNoThrow;
    }
}
