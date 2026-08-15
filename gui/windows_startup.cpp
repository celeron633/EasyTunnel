#include "windows_startup.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <windows.h>
#include <lmcons.h>
#include <security.h>
#include <taskschd.h>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kTaskName[] = L"EasyTunnel GUI Startup";
constexpr wchar_t kTaskAuthor[] = L"EasyTunnel";
constexpr wchar_t kTaskDescription[] =
    L"Starts the EasyTunnel GUI when this user signs in.";

template <typename T>
class ComPtr {
public:
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr() = default;

    T* Get() const { return value_; }
    T* operator->() const { return value_; }

    T** Receive() {
        Reset();
        return &value_;
    }

private:
    void Reset() {
        if (value_) value_->Release();
        value_ = nullptr;
    }

    T* value_ = nullptr;
};

class ScopedCom {
public:
    ScopedCom() {
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        shouldUninitialize_ = SUCCEEDED(result_);
    }

    ~ScopedCom() {
        if (shouldUninitialize_) CoUninitialize();
    }

    bool IsReady() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    HRESULT Result() const { return result_; }

private:
    HRESULT result_ = E_FAIL;
    bool shouldUninitialize_ = false;
};

class ScopedBstr {
public:
    explicit ScopedBstr(const wchar_t* value) : value_(SysAllocString(value)) {}
    explicit ScopedBstr(const std::wstring& value)
        : value_(SysAllocStringLen(value.data(), static_cast<UINT>(value.size()))) {}
    ~ScopedBstr() { SysFreeString(value_); }

    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    BSTR Get() const { return value_; }
    bool IsValid() const { return value_ != nullptr; }

private:
    BSTR value_ = nullptr;
};

void SetHresultError(const char* operation, HRESULT result, std::string* error) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX",
                  static_cast<unsigned long>(result));
    *error = std::string(operation) + " failed (HRESULT " + code + ")";
}

bool CheckResult(HRESULT result, const char* operation, std::string* error) {
    if (SUCCEEDED(result)) return true;
    SetHresultError(operation, result, error);
    return false;
}

bool GetCurrentExecutablePath(std::wstring* path, std::string* error) {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            *error = "Cannot get the EasyTunnel executable path (Windows error "
                + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (length < buffer.size()) {
            path->assign(buffer.data(), length);
            return true;
        }
        buffer.resize(buffer.size() * 2);
    }
    *error = "The EasyTunnel executable path is too long for Windows startup";
    return false;
}

bool GetCurrentUserName(std::wstring* userName, std::string* error) {
    ULONG length = 0;
    GetUserNameExW(NameSamCompatible, nullptr, &length);
    if (length != 0) {
        std::vector<wchar_t> buffer(length);
        if (GetUserNameExW(NameSamCompatible, buffer.data(), &length)) {
            userName->assign(buffer.data());
            return true;
        }
    }

    std::vector<wchar_t> fallbackBuffer(UNLEN + 1);
    DWORD fallbackLength = static_cast<DWORD>(fallbackBuffer.size());
    if (GetUserNameW(fallbackBuffer.data(), &fallbackLength)) {
        userName->assign(fallbackBuffer.data(), fallbackLength);
        return true;
    }

    *error = "Cannot determine the Windows user for startup (Windows error "
        + std::to_string(GetLastError()) + ")";
    return false;
}

bool OpenTaskScheduler(ComPtr<ITaskService>* service,
                       ComPtr<ITaskFolder>* root,
                       std::string* error) {
    HRESULT result = CoCreateInstance(
        CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITaskService, reinterpret_cast<void**>(service->Receive()));
    if (!CheckResult(result, "Opening Windows Task Scheduler", error)) return false;

    VARIANT empty;
    VariantInit(&empty);
    result = service->Get()->Connect(empty, empty, empty, empty);
    if (!CheckResult(result, "Connecting to Windows Task Scheduler", error)) {
        return false;
    }

    ScopedBstr rootPath(L"\\");
    if (!rootPath.IsValid()) {
        *error = "Cannot allocate the Windows startup task path";
        return false;
    }
    result = service->Get()->GetFolder(rootPath.Get(), root->Receive());
    return CheckResult(result, "Opening the Task Scheduler Library", error);
}

bool PathsMatch(const std::wstring& left, const std::wstring& right) {
    if (left.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
        || right.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()), TRUE)
        == CSTR_EQUAL;
}

bool RegisteredTaskUsesExecutable(IRegisteredTask* task,
                                  const std::wstring& executablePath,
                                  bool* matches,
                                  std::string* error) {
    *matches = false;
    VARIANT_BOOL taskEnabled = VARIANT_FALSE;
    HRESULT result = task->get_Enabled(&taskEnabled);
    if (!CheckResult(result, "Reading the EasyTunnel startup task", error)) {
        return false;
    }
    if (taskEnabled != VARIANT_TRUE) return true;

    ComPtr<ITaskDefinition> definition;
    result = task->get_Definition(definition.Receive());
    if (!CheckResult(result, "Reading the EasyTunnel startup definition", error)) {
        return false;
    }

    ComPtr<IActionCollection> actions;
    result = definition->get_Actions(actions.Receive());
    if (!CheckResult(result, "Reading the EasyTunnel startup action", error)) {
        return false;
    }

    LONG actionCount = 0;
    result = actions->get_Count(&actionCount);
    if (!CheckResult(result, "Reading the EasyTunnel startup action", error)) {
        return false;
    }
    for (LONG index = 1; index <= actionCount; ++index) {
        ComPtr<IAction> action;
        result = actions->get_Item(index, action.Receive());
        if (!CheckResult(result, "Reading the EasyTunnel startup action", error)) {
            return false;
        }
        TASK_ACTION_TYPE actionType = TASK_ACTION_COM_HANDLER;
        result = action->get_Type(&actionType);
        if (!CheckResult(result, "Reading the EasyTunnel startup action", error)) {
            return false;
        }
        if (actionType != TASK_ACTION_EXEC) continue;

        ComPtr<IExecAction> execAction;
        result = action->QueryInterface(
            IID_IExecAction, reinterpret_cast<void**>(execAction.Receive()));
        if (!CheckResult(result, "Reading the EasyTunnel startup executable", error)) {
            return false;
        }
        BSTR registeredPath = nullptr;
        result = execAction->get_Path(&registeredPath);
        if (!CheckResult(result, "Reading the EasyTunnel startup executable", error)) {
            return false;
        }
        const std::wstring path(registeredPath ? registeredPath : L"");
        SysFreeString(registeredPath);
        *matches = PathsMatch(path, executablePath);
        return true;
    }
    return true;
}

bool ConfigureTaskSettings(ITaskDefinition* definition, std::string* error) {
    ComPtr<IRegistrationInfo> registration;
    HRESULT result = definition->get_RegistrationInfo(registration.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel startup task", error)) {
        return false;
    }
    ScopedBstr author(kTaskAuthor);
    ScopedBstr description(kTaskDescription);
    if (!author.IsValid() || !description.IsValid()) {
        *error = "Cannot allocate the EasyTunnel startup task metadata";
        return false;
    }
    result = registration->put_Author(author.Get());
    if (!CheckResult(result, "Setting the startup task author", error)) return false;
    result = registration->put_Description(description.Get());
    if (!CheckResult(result, "Setting the startup task description", error)) {
        return false;
    }

    ComPtr<IPrincipal> principal;
    result = definition->get_Principal(principal.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel startup principal", error)) {
        return false;
    }
    result = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    if (!CheckResult(result, "Setting the startup logon type", error)) return false;
    result = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    if (!CheckResult(result, "Setting the startup privilege level", error)) {
        return false;
    }

    ComPtr<ITaskSettings> settings;
    result = definition->get_Settings(settings.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel startup settings", error)) {
        return false;
    }
    result = settings->put_Enabled(VARIANT_TRUE);
    if (!CheckResult(result, "Enabling the EasyTunnel startup task", error)) {
        return false;
    }
    result = settings->put_StartWhenAvailable(VARIANT_TRUE);
    if (!CheckResult(result, "Setting the startup task availability", error)) {
        return false;
    }
    result = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    if (!CheckResult(result, "Allowing startup while on battery", error)) return false;
    result = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    if (!CheckResult(result, "Allowing EasyTunnel to stay running on battery", error)) {
        return false;
    }
    result = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    if (!CheckResult(result, "Setting the startup instance policy", error)) {
        return false;
    }
    ScopedBstr noTimeLimit(L"PT0S");
    if (!noTimeLimit.IsValid()) {
        *error = "Cannot allocate the EasyTunnel startup time limit";
        return false;
    }
    result = settings->put_ExecutionTimeLimit(noTimeLimit.Get());
    return CheckResult(result, "Removing the startup task time limit", error);
}

bool AddLogonTrigger(ITaskDefinition* definition,
                     const std::wstring& userName,
                     std::string* error) {
    ComPtr<ITriggerCollection> triggers;
    HRESULT result = definition->get_Triggers(triggers.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel logon trigger", error)) {
        return false;
    }
    ComPtr<ITrigger> trigger;
    result = triggers->Create(TASK_TRIGGER_LOGON, trigger.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel logon trigger", error)) {
        return false;
    }
    ComPtr<ILogonTrigger> logonTrigger;
    result = trigger->QueryInterface(
        IID_ILogonTrigger, reinterpret_cast<void**>(logonTrigger.Receive()));
    if (!CheckResult(result, "Creating the EasyTunnel logon trigger", error)) {
        return false;
    }
    ScopedBstr triggerId(L"EasyTunnelLogon");
    ScopedBstr triggerUser(userName);
    if (!triggerId.IsValid() || !triggerUser.IsValid()) {
        *error = "Cannot allocate the EasyTunnel logon trigger";
        return false;
    }
    result = logonTrigger->put_Id(triggerId.Get());
    if (!CheckResult(result, "Naming the EasyTunnel logon trigger", error)) {
        return false;
    }
    result = logonTrigger->put_UserId(triggerUser.Get());
    return CheckResult(result, "Setting the EasyTunnel startup user", error);
}

bool AddExecutableAction(ITaskDefinition* definition,
                         const std::wstring& executablePath,
                         std::string* error) {
    ComPtr<IActionCollection> actions;
    HRESULT result = definition->get_Actions(actions.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel startup action", error)) {
        return false;
    }
    ComPtr<IAction> action;
    result = actions->Create(TASK_ACTION_EXEC, action.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel startup action", error)) {
        return false;
    }
    ComPtr<IExecAction> execAction;
    result = action->QueryInterface(
        IID_IExecAction, reinterpret_cast<void**>(execAction.Receive()));
    if (!CheckResult(result, "Creating the EasyTunnel startup action", error)) {
        return false;
    }

    const size_t slash = executablePath.find_last_of(L"\\/");
    const std::wstring workingDirectory = slash == std::wstring::npos
        ? std::wstring()
        : executablePath.substr(0, slash);
    ScopedBstr path(executablePath);
    ScopedBstr directory(workingDirectory);
    if (!path.IsValid() || !directory.IsValid()) {
        *error = "Cannot allocate the EasyTunnel startup executable path";
        return false;
    }
    result = execAction->put_Path(path.Get());
    if (!CheckResult(result, "Setting the EasyTunnel startup executable", error)) {
        return false;
    }
    result = execAction->put_WorkingDirectory(directory.Get());
    return CheckResult(result, "Setting the EasyTunnel startup directory", error);
}

bool RegisterStartupTask(ITaskService* service, ITaskFolder* root,
                         const std::wstring& executablePath,
                         const std::wstring& userName,
                         std::string* error) {
    ComPtr<ITaskDefinition> definition;
    HRESULT result = service->NewTask(0, definition.Receive());
    if (!CheckResult(result, "Creating the EasyTunnel startup task", error)) {
        return false;
    }
    if (!ConfigureTaskSettings(definition.Get(), error)
        || !AddLogonTrigger(definition.Get(), userName, error)
        || !AddExecutableAction(definition.Get(), executablePath, error)) {
        return false;
    }

    ScopedBstr taskName(kTaskName);
    ScopedBstr taskUser(userName);
    if (!taskName.IsValid() || !taskUser.IsValid()) {
        *error = "Cannot allocate the EasyTunnel startup registration";
        return false;
    }
    VARIANT user;
    VariantInit(&user);
    user.vt = VT_BSTR;
    user.bstrVal = taskUser.Get();
    VARIANT empty;
    VariantInit(&empty);
    ComPtr<IRegisteredTask> registeredTask;
    result = root->RegisterTaskDefinition(
        taskName.Get(), definition.Get(), TASK_CREATE_OR_UPDATE,
        user, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty,
        registeredTask.Receive());
    return CheckResult(result, "Registering the EasyTunnel startup task", error);
}
}  // namespace

bool IsWindowsStartupEnabled(bool* enabled, std::string* error) {
    if (!enabled || !error) return false;
    *enabled = false;
    error->clear();

    std::wstring executablePath;
    if (!GetCurrentExecutablePath(&executablePath, error)) return false;

    ScopedCom com;
    if (!com.IsReady()) {
        SetHresultError("Initializing Windows startup support", com.Result(), error);
        return false;
    }
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!OpenTaskScheduler(&service, &root, error)) return false;

    ScopedBstr taskName(kTaskName);
    if (!taskName.IsValid()) {
        *error = "Cannot allocate the EasyTunnel startup task name";
        return false;
    }
    ComPtr<IRegisteredTask> task;
    const HRESULT result = root->GetTask(taskName.Get(), task.Receive());
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        return true;
    }
    if (!CheckResult(result, "Reading the EasyTunnel startup task", error)) {
        return false;
    }
    return RegisteredTaskUsesExecutable(
        task.Get(), executablePath, enabled, error);
}

bool SetWindowsStartupEnabled(bool enabled, std::string* error) {
    if (!error) return false;
    error->clear();

    ScopedCom com;
    if (!com.IsReady()) {
        SetHresultError("Initializing Windows startup support", com.Result(), error);
        return false;
    }
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!OpenTaskScheduler(&service, &root, error)) return false;

    ScopedBstr taskName(kTaskName);
    if (!taskName.IsValid()) {
        *error = "Cannot allocate the EasyTunnel startup task name";
        return false;
    }
    if (!enabled) {
        const HRESULT result = root->DeleteTask(taskName.Get(), 0);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            return true;
        }
        return CheckResult(result, "Removing the EasyTunnel startup task", error);
    }

    std::wstring executablePath;
    std::wstring userName;
    if (!GetCurrentExecutablePath(&executablePath, error)
        || !GetCurrentUserName(&userName, error)) {
        return false;
    }
    return RegisterStartupTask(
        service.Get(), root.Get(), executablePath, userName, error);
}
