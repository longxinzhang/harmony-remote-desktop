#include <napi/native_api.h>
#include <fstream>
#include <cstdio>
#include <string>
#include <utility>
#include "capture_probe.h"
#include "encoder_probe.h"
#include "input_probe.h"
#include "lan_server.h"
#include "remote_input.h"

namespace {
napi_value Number(napi_env env, int code)
{
    napi_value result;
    napi_create_int32(env, code, &result);
    return result;
}

napi_value Text(napi_env env, const std::string& text)
{
    napi_value result;
    napi_create_string_utf8(env, text.c_str(), text.size(), &result);
    return result;
}

bool ReadText(napi_env env, napi_value value, std::string& result)
{
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok || length > 8192) {
        napi_throw_type_error(env, nullptr, "Expected a string of at most 8192 bytes");
        return false;
    }
    result.resize(length + 1);
    napi_get_value_string_utf8(env, value, result.data(), result.size(), &length);
    result.resize(length);
    return true;
}

napi_value StartCapture(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string filesDir;
    if (argc != 1 || !ReadText(env, args[0], filesDir)) { return nullptr; }
    GetEncoderProbe().Stop();
    return Number(env, GetCaptureProbe().Start(filesDir));
}

napi_value StartEncoder(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string filesDir;
    if (argc != 1 || !ReadText(env, args[0], filesDir)) { return nullptr; }
    GetCaptureProbe().Stop();
    return Number(env, GetEncoderProbe().Start(filesDir));
}

napi_value StopEncoder(napi_env env, napi_callback_info)
{
    GetRemoteInput().EnableSession(false);
    GetEncoderProbe().Stop();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

napi_value EncoderSnapshot(napi_env env, napi_callback_info)
{
    return Text(env, GetEncoderProbe().SnapshotJson());
}

napi_value StartLanServer(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string address;
    if (argc != 1 || !ReadText(env, args[0], address)) { return nullptr; }
    GetRemoteInput().SetAllowed(false);
    return Number(env, GetLanServer().Start(address));
}

napi_value StopLanServer(napi_env env, napi_callback_info)
{
    GetRemoteInput().SetAllowed(false);
    // Stop the producer before closing its non-blocking stream sink.
    GetEncoderProbe().Stop();
    GetLanServer().Stop();
    GetInputProbe().CancelAndRelease();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

napi_value LanSnapshot(napi_env env, napi_callback_info)
{
    // PIN is only returned for the foreground UI, never diagnostic files.
    return Text(env, GetLanServer().SnapshotJson(true));
}

napi_value StartLanCapture(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string filesDir;
    double duration = 0;
    if (argc != 2 || !ReadText(env, args[0], filesDir) ||
        napi_get_value_double(env, args[1], &duration) != napi_ok ||
        !encoder_session::IsLanDuration(duration)) { return Number(env, -1); }
    if (GetEncoderProbe().IsRunning()) { return Number(env, -2); }
    if (!GetLanServer().BeginStream()) { return Number(env, -4); }
    GetCaptureProbe().Stop();
    EncoderStreamHooks hooks;
    hooks.packet = [](const uint8_t* data, size_t size, uint64_t ptsUs,
                      bool config, bool keyframe, bool eos) {
        return GetLanServer().Publish(data, size, ptsUs, config, keyframe, eos);
    };
    hooks.cancelled = [] { return GetLanServer().IsStreamCancelled(); };
    hooks.finished = [](bool success) { GetLanServer().EndStream(success); };
    EncoderSessionOptions options;
    options.durationSeconds = static_cast<int>(duration);
    options.recordLocally = false;
    const int code = GetEncoderProbe().Start(filesDir, std::move(hooks), options);
    return Number(env, code);
}

napi_value StopCapture(napi_env env, napi_callback_info)
{
    GetCaptureProbe().Stop();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

napi_value CaptureSnapshot(napi_env env, napi_callback_info)
{
    return Text(env, GetCaptureProbe().SnapshotJson());
}
napi_value InputSnapshot(napi_env env, napi_callback_info)
{
    return Text(env, GetInputProbe().QueryJson());
}
napi_value RequestInput(napi_env env, napi_callback_info)
{
    GetRemoteInput().SetAllowed(false);
    return Number(env, GetInputProbe().RequestAuthorization());
}
napi_value MoveMouse(napi_env env, napi_callback_info)
{
    return Number(env, GetInputProbe().MoveMouseToCenter());
}
napi_value InjectA(napi_env env, napi_callback_info)
{
    return Number(env, GetInputProbe().InjectA());
}
napi_value ClickLeft(napi_env env, napi_callback_info)
{
    return Number(env, GetInputProbe().ClickLeft());
}
napi_value InjectCtrlL(napi_env env, napi_callback_info)
{
    return Number(env, GetInputProbe().InjectCtrlL());
}
napi_value CancelInput(napi_env env, napi_callback_info)
{
    GetRemoteInput().SetAllowed(false);
    return Number(env, GetInputProbe().CancelAndRelease());
}

napi_value RemoteInputSnapshot(napi_env env, napi_callback_info)
{
    return Text(env, GetRemoteInput().SnapshotJson());
}

napi_value AllowRemoteInput(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool allowed = false;
    if (argc != 1 || napi_get_value_bool(env, args[0], &allowed) != napi_ok) {
        return Number(env, -1);
    }
    // User-facing local action only. Network enable cannot grant Host consent.
    GetRemoteInput().SetAllowed(allowed);
    return Number(env, 0);
}

bool WriteFile(const std::string& path, const std::string& data)
{
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << data << '\n';
    output.close();
    if (output.fail() || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

napi_value SaveDiagnostics(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string filesDir, deviceJson;
    if (argc != 2 || !ReadText(env, args[0], filesDir) || !ReadText(env, args[1], deviceJson)) {
        return nullptr;
    }
    if (filesDir.empty() || filesDir.front() != '/' || filesDir.find('\0') != std::string::npos) {
        return Number(env, -1);
    }
    // Separate from the capture worker's own report so manual export cannot
    // race its final write. No pixels or recording buffers cross N-API.
    bool success = WriteFile(filesDir + "/input-test.json", GetInputProbe().QueryJson());
    success = WriteFile(filesDir + "/capture-snapshot.json", GetCaptureProbe().SnapshotJson()) && success;
    success = WriteFile(filesDir + "/encoder-snapshot.json", GetEncoderProbe().SnapshotJson()) && success;
    success = WriteFile(filesDir + "/lan-snapshot.json", GetLanServer().SnapshotJson()) && success;
    success = WriteFile(filesDir + "/remote-input.json", GetRemoteInput().SnapshotJson()) && success;
    success = WriteFile(filesDir + "/device-info.json", deviceJson) && success;
    return Number(env, success ? 0 : -2);
}

void Cleanup(void*)
{
    GetRemoteInput().SetAllowed(false);
    GetCaptureProbe().Stop();
    GetEncoderProbe().Stop();
    GetLanServer().Stop();
    GetInputProbe().CancelAndRelease();
}

napi_value Init(napi_env env, napi_value exports)
{
    // The stream sink must outlive the encoder's worker during static teardown.
    // Construction never starts listeners; only the foreground Start button does.
    GetRemoteInput();
    LanInputHooks inputHooks;
    inputHooks.enable = [](bool enabled) { return GetRemoteInput().EnableSession(enabled); };
    inputHooks.enabled = [] { return GetRemoteInput().IsSessionEnabled(); };
    inputHooks.submit = [](const RemoteInputEvent& event) { return GetRemoteInput().Submit(event); };
    inputHooks.release = [] { GetRemoteInput().ReleaseAll(); };
    GetLanServer().SetInputHooks(std::move(inputHooks));
    GetEncoderProbe();
    napi_property_descriptor methods[] = {
        {"startCapture", nullptr, StartCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopCapture", nullptr, StopCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"captureSnapshot", nullptr, CaptureSnapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startEncoder", nullptr, StartEncoder, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopEncoder", nullptr, StopEncoder, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"encoderSnapshot", nullptr, EncoderSnapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startLanServer", nullptr, StartLanServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopLanServer", nullptr, StopLanServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"lanSnapshot", nullptr, LanSnapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"remoteInputSnapshot", nullptr, RemoteInputSnapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"allowRemoteInput", nullptr, AllowRemoteInput, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startLanCapture", nullptr, StartLanCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"inputSnapshot", nullptr, InputSnapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"requestInputAuthorization", nullptr, RequestInput, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"moveMouseToCenter", nullptr, MoveMouse, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"injectA", nullptr, InjectA, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clickLeft", nullptr, ClickLeft, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"injectCtrlL", nullptr, InjectCtrlL, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cancelInput", nullptr, CancelInput, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"saveDiagnostics", nullptr, SaveDiagnostics, nullptr, nullptr, nullptr, napi_default, nullptr}
    };
    napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods);
    napi_add_env_cleanup_hook(env, Cleanup, nullptr);
    return exports;
}

napi_module module = {1, 0, nullptr, Init, "hrd_probe", nullptr, {0}};
}

extern "C" __attribute__((constructor)) void RegisterHrdProbe()
{
    napi_module_register(&module);
}
