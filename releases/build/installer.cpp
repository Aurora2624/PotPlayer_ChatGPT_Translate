#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "resource.h"
#include "generated/installer_generated.h"

#ifndef ID_WIZBACK
#define ID_WIZBACK 0x3023
#endif
#ifndef ID_WIZNEXT
#define ID_WIZNEXT 0x3024
#endif
#ifndef ID_WIZFINISH
#define ID_WIZFINISH 0x3025
#endif

namespace {

constexpr double kVerifyRequestTimeoutSeconds = 15.0;
constexpr int kVerifyRequestMaxRetries = 0;

constexpr UINT WM_APP_INSTALL_LOG = WM_APP + 1;
constexpr UINT WM_APP_INSTALL_DONE = WM_APP + 2;
constexpr UINT WM_APP_INSTALL_PROMPT = WM_APP + 3;

constexpr COLORREF kBgColor = RGB(30, 30, 30);
constexpr COLORREF kEditColor = RGB(25, 25, 25);
constexpr COLORREF kTextColor = RGB(220, 220, 220);

constexpr const wchar_t* kLanguageEn = L"en";
constexpr const wchar_t* kLanguageZh = L"zh";
constexpr const wchar_t* kVariantWithContext = L"with_context";
constexpr const wchar_t* kVariantWithoutContext = L"without_context";

struct ProviderConfig {
    std::wstring key;
    std::wstring model;
    std::wstring api_base;
    std::wstring purchase_page;
    bool allow_custom_model = false;
};

struct PresetEntry {
    std::wstring label;
    std::wstring provider_key;
    std::vector<std::wstring> aliases;
};

struct EmbeddedFileSpec {
    int resource_id;
    std::wstring dest_name;
    bool is_text;
};

struct WizardState {
    std::wstring language = kLanguageEn;
    std::wstring install_dir;
    std::vector<std::wstring> versions{ kVariantWithContext };
    std::wstring api_key;
    std::wstring model = L"gpt-5-nano";
    std::wstring api_base = L"https://api.openai.com/v1/chat/completions";
    int delay_ms = 500;
    int retry_mode = 1;
    bool debug_mode = false;
    bool check_hallucination = false;
    int context_subtitle_count = 3;
    std::wstring context_cache_mode = L"auto";
    std::wstring prompt_cache_retention = L"24h";
    std::wstring gemini_cached_content;
    bool small_model = false;
    bool has_context_variant = true;
};

struct RegInfo {
    std::wstring key;
    std::wstring version;
    std::wstring uninstall;
    std::wstring context;
};

enum class InstallPromptType {
    FileExists,
    YesNo,
    Text,
};

struct InstallPromptRequest {
    InstallPromptType type;
    std::wstring title;
    std::wstring message;
    int choice_result = 0;
    bool bool_result = false;
    bool accepted = false;
    std::wstring text_result;
    HANDLE event_handle = nullptr;
};

enum class InstallStatus {
    Success,
    Failed,
    Cancelled,
};

struct InstallThreadData {
    WizardState state;
    HWND progress_hwnd = nullptr;
};

struct ConfigUiState {
    std::vector<PresetEntry> presets;
    std::wstring purchase_link;
    bool skip_requested = false;
    bool empty_key_verified = false;
};

struct ProgressUiState {
    HWND hwnd = nullptr;
    HANDLE thread_handle = nullptr;
    bool started = false;
    bool completed = false;
    bool succeeded = false;
};

struct WizardContext {
    HINSTANCE instance = nullptr;
    WizardState state;
    ConfigUiState config_ui;
    ProgressUiState progress_ui;
    HBRUSH bg_brush = nullptr;
    HBRUSH edit_brush = nullptr;
};

struct InputDialogState {
    WizardContext* ctx = nullptr;
    std::wstring title;
    std::wstring prompt;
    std::wstring value;
    bool accepted = false;
};

WizardContext* g_ctx = nullptr;

template <typename T>
T* GetDialogState(HWND hwnd) {
    return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

template <typename T>
void SetDialogState(HWND hwnd, T* value) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(value));
}

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring output(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring Trim(std::wstring value) {
    const auto not_space = [](wchar_t ch) { return !iswspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

bool EndsWith(std::wstring_view value, std::wstring_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

bool StartsWith(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

void ReplaceAll(std::wstring& text, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::wstring FormatOneArg(const std::wstring& pattern, const std::wstring& argument) {
    std::wstring output = pattern;
    ReplaceAll(output, L"{}", argument);
    return output;
}

std::wstring ToWindowsNewlines(std::wstring text) {
    ReplaceAll(text, L"\r\n", L"\n");
    ReplaceAll(text, L"\r", L"\n");
    ReplaceAll(text, L"\n", L"\r\n");
    return text;
}

std::wstring GetEnvVar(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring value(size - 1, L'\0');
    GetEnvironmentVariableW(name, value.data(), size);
    return value;
}

const std::unordered_map<std::wstring, std::wstring>& StringsForLanguage(const std::wstring& language) {
    return kLanguageStrings.at(language);
}

const std::wstring& S(const std::wstring& language, const std::wstring& key) {
    return StringsForLanguage(language).at(key);
}

const std::wstring& S(const WizardContext& ctx, const std::wstring& key) {
    return S(ctx.state.language, key);
}

std::wstring MergeBilingual(const std::wstring& key) {
    return S(kLanguageEn, key) + L"\n\n" + S(kLanguageZh, key);
}

void EnableDarkTitleBar(HWND hwnd) {
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &enabled, sizeof(enabled));
    DwmSetWindowAttribute(hwnd, 19, &enabled, sizeof(enabled));
}

INT_PTR HandleDarkColorMessage(WPARAM wParam, UINT message, WizardContext* ctx) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, kTextColor);
    SetBkMode(dc, OPAQUE);

    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        SetBkColor(dc, kEditColor);
        return reinterpret_cast<INT_PTR>(ctx->edit_brush);
    }

    SetBkColor(dc, kBgColor);
    return reinterpret_cast<INT_PTR>(ctx->bg_brush);
}

void SetText(HWND hwnd, int control_id, const std::wstring& value) {
    SetDlgItemTextW(hwnd, control_id, value.c_str());
}

std::wstring GetText(HWND hwnd, int control_id) {
    const int len = GetWindowTextLengthW(GetDlgItem(hwnd, control_id));
    std::wstring text(len, L'\0');
    GetDlgItemTextW(hwnd, control_id, text.data(), len + 1);
    return text;
}

int GetIntText(HWND hwnd, int control_id, int fallback) {
    BOOL ok = FALSE;
    const UINT value = GetDlgItemInt(hwnd, control_id, &ok, FALSE);
    return ok ? static_cast<int>(value) : fallback;
}

void SetIntText(HWND hwnd, int control_id, int value) {
    SetDlgItemInt(hwnd, control_id, static_cast<UINT>(value), FALSE);
}

void ShowSimpleMessage(HWND owner, const std::wstring& title, const std::wstring& message, UINT flags = MB_OK | MB_ICONINFORMATION) {
    MessageBoxW(owner, ToWindowsNewlines(message).c_str(), title.c_str(), flags);
}

std::wstring GetLastErrorMessage(DWORD error = GetLastError()) {
    LPWSTR buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message = len > 0 && buffer ? std::wstring(buffer, len) : L"Unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    return Trim(message);
}

std::filesystem::path ModuleDir() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (len == path.size()) {
        path.resize(path.size() * 2);
        len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(len);
    return std::filesystem::path(path).parent_path();
}

struct ResourceBytes {
    const std::byte* data = nullptr;
    size_t size = 0;
};

std::optional<ResourceBytes> LoadResourceBytes(int resource_id) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource) {
        return std::nullopt;
    }
    HGLOBAL loaded = LoadResource(nullptr, resource);
    if (!loaded) {
        return std::nullopt;
    }
    const auto size = static_cast<size_t>(SizeofResource(nullptr, resource));
    const void* ptr = LockResource(loaded);
    if (!ptr || size == 0) {
        return std::nullopt;
    }
    return ResourceBytes{ reinterpret_cast<const std::byte*>(ptr), size };
}

std::string LoadResourceTextUtf8(int resource_id) {
    auto bytes = LoadResourceBytes(resource_id);
    if (!bytes) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes->data), bytes->size);
}

bool WriteBytesToFile(const std::filesystem::path& path, const std::byte* data, size_t size) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
}

bool WriteTextUtf8NoBom(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

bool EnsureDirExists(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return true;
    }
    return std::filesystem::create_directories(path, ec);
}

const std::vector<ProviderConfig>& ApiProviders() {
    static const std::vector<ProviderConfig> providers = {
        {L"gpt-5", L"gpt-5", L"https://api.openai.com/v1/chat/completions", L"https://platform.openai.com/account/billing", false},
        {L"gpt-5-mini", L"gpt-5-mini", L"https://api.openai.com/v1/chat/completions", L"https://platform.openai.com/account/billing", false},
        {L"gpt-5-nano", L"gpt-5-nano", L"https://api.openai.com/v1/chat/completions", L"https://platform.openai.com/account/billing", false},
        {L"gpt-4o", L"gpt-4o", L"https://api.openai.com/v1/chat/completions", L"https://platform.openai.com/account/billing", false},
        {L"gpt-4.1", L"gpt-4.1", L"https://api.openai.com/v1/chat/completions", L"https://platform.openai.com/account/billing", false},
        {L"gpt-4.1-mini", L"gpt-4.1-mini", L"https://api.openai.com/v1/chat/completions", L"https://platform.openai.com/account/billing", false},
        {L"ollama", L"glm-4", L"http://xxx.xxx.xxx.xxx:11434/v1/chat/completions", L"pass", true},
        {L"gemini-flash", L"gemini-3-flash-preview", L"https://generativelanguage.googleapis.com/v1beta/openai/", L"https://aistudio.google.com/app/apikey", false},
        {L"__CUSTOM__", L"", L"", L"", false},
    };
    return providers;
}

const ProviderConfig* FindProvider(const std::wstring& key) {
    for (const auto& provider : ApiProviders()) {
        if (provider.key == key) {
            return &provider;
        }
    }
    return nullptr;
}

std::wstring NormalizeApiUrlForConfig(const std::wstring& api_url) {
    std::wstring value = Trim(api_url);
    if (value.empty()) {
        return L"https://api.openai.com/v1/chat/completions";
    }
    while (!value.empty() && value.back() == L'/') {
        value.pop_back();
    }
    if (EndsWith(value, L"/chat/completions") || EndsWith(value, L"/responses")) {
        return value;
    }
    if (std::regex_search(value, std::wregex(L"/v[14]$"))) {
        return value + L"/chat/completions";
    }
    return value;
}

std::wstring NormalizeBaseUrlForOpenAI(const std::wstring& api_url) {
    std::wstring value = Trim(api_url);
    while (!value.empty() && value.back() == L'/') {
        value.pop_back();
    }
    if (value.empty()) {
        return L"https://api.openai.com/v1";
    }
    if (EndsWith(value, L"/chat/completions")) {
        value.erase(value.size() - std::wstring(L"/chat/completions").size());
        return value;
    }
    if (EndsWith(value, L"/responses")) {
        value.erase(value.size() - std::wstring(L"/responses").size());
        return value;
    }
    return value;
}

std::vector<PresetEntry> BuildProviderPresetEntries() {
    std::vector<PresetEntry> entries;
    std::unordered_map<std::wstring, size_t> signature_to_index;
    for (const auto& provider : ApiProviders()) {
        if (provider.key == L"__CUSTOM__") {
            continue;
        }
        const std::wstring signature =
            provider.model + L"|" + NormalizeApiUrlForConfig(provider.api_base) + L"|" + (provider.allow_custom_model ? L"1" : L"0");
        auto found = signature_to_index.find(signature);
        if (found == signature_to_index.end()) {
            PresetEntry entry;
            entry.label = provider.key;
            entry.provider_key = provider.key;
            entry.aliases.push_back(provider.key);
            signature_to_index.emplace(signature, entries.size());
            entries.push_back(std::move(entry));
        } else {
            auto& entry = entries[found->second];
            entry.aliases.push_back(provider.key);
            entry.label.clear();
            for (size_t i = 0; i < entry.aliases.size(); ++i) {
                if (i > 0) {
                    entry.label += L" / ";
                }
                entry.label += entry.aliases[i];
            }
        }
    }
    return entries;
}

std::wstring EscapeJsonString(const std::wstring& value) {
    std::wstring output;
    output.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += L"\\\\"; break;
        case L'"': output += L"\\\""; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': output += L"\\r"; break;
        case L'\t': output += L"\\t"; break;
        default: output += ch; break;
        }
    }
    return output;
}

std::wstring EscapeForAsString(const std::wstring& value) {
    std::wstring output;
    output.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += L"\\\\"; break;
        case L'"': output += L"\\\""; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': break;
        default: output += ch; break;
        }
    }
    return output;
}

bool ReplaceQuotedValue(std::string& data, const std::string& key, const std::string& value) {
    const std::regex pattern("(" + key + R"(\s*=\s*"))(.*?)(")");
    std::smatch match;
    if (!std::regex_search(data, match, pattern)) {
        return false;
    }
    data.replace(static_cast<size_t>(match.position(2)), static_cast<size_t>(match.length(2)), value);
    return true;
}

bool IsAdmin() {
    BOOL admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &admin);
        FreeSid(admin_group);
    }
    return admin == TRUE;
}

void RestartAsAdmin() {
    std::wstring parameters;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (!parameters.empty()) {
                parameters += L' ';
            }
            parameters += L'"';
            parameters += argv[i];
            parameters += L'"';
        }
        LocalFree(argv);
    }
    std::wstring exe_path(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, exe_path.data(), static_cast<DWORD>(exe_path.size()));
    exe_path.resize(len);
    ShellExecuteW(nullptr, L"runas", exe_path.c_str(), parameters.empty() ? nullptr : parameters.c_str(), nullptr, SW_SHOWNORMAL);
}

std::optional<std::filesystem::path> ResolveShortcutTarget(const std::filesystem::path& shortcut_path) {
    IShellLinkW* shell_link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link));
    if (FAILED(hr) || !shell_link) {
        return std::nullopt;
    }

    IPersistFile* persist = nullptr;
    hr = shell_link->QueryInterface(IID_PPV_ARGS(&persist));
    if (FAILED(hr) || !persist) {
        shell_link->Release();
        return std::nullopt;
    }

    std::optional<std::filesystem::path> result;
    hr = persist->Load(shortcut_path.c_str(), STGM_READ);
    if (SUCCEEDED(hr)) {
        wchar_t target[MAX_PATH] = {};
        WIN32_FIND_DATAW find_data{};
        hr = shell_link->GetPath(target, MAX_PATH, &find_data, SLGP_RAWPATH);
        if (SUCCEEDED(hr) && target[0] != L'\0') {
            result = std::filesystem::path(target);
        }
    }

    persist->Release();
    shell_link->Release();
    return result;
}

std::vector<std::filesystem::path> SearchDirsForShortcuts() {
    std::vector<std::filesystem::path> dirs;
    const std::wstring user_profile = GetEnvVar(L"USERPROFILE");
    const std::wstring app_data = GetEnvVar(L"APPDATA");
    if (!user_profile.empty()) {
        dirs.emplace_back(std::filesystem::path(user_profile) / L"Desktop");
    }
    if (!app_data.empty()) {
        dirs.emplace_back(std::filesystem::path(app_data) / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs");
    }
    dirs.emplace_back(LR"(C:\Users\Public\Desktop)");
    dirs.emplace_back(LR"(C:\ProgramData\Microsoft\Windows\Start Menu\Programs)");
    return dirs;
}

std::optional<std::filesystem::path> ScanShortcuts() {
    for (const auto& base : SearchDirsForShortcuts()) {
        std::error_code ec;
        if (!std::filesystem::exists(base, ec)) {
            continue;
        }
        for (std::filesystem::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                continue;
            }
            if (!it->is_regular_file(ec)) {
                continue;
            }
            const auto filename_lower = ToLower(it->path().filename().wstring());
            if (!EndsWith(filename_lower, L".lnk") || filename_lower.find(L"potplayer") == std::wstring::npos) {
                continue;
            }
            auto target = ResolveShortcutTarget(it->path());
            if (!target || !std::filesystem::exists(*target, ec)) {
                continue;
            }
            const auto translate_dir = target->parent_path() / L"Extension" / L"Subtitle" / L"Translate";
            if (std::filesystem::exists(translate_dir, ec)) {
                return translate_dir;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> ExistingDriveRoots() {
    std::vector<std::filesystem::path> drives;
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        wchar_t root[] = { letter, L':', L'\\', L'\0' };
        if (GetDriveTypeW(root) != DRIVE_NO_ROOT_DIR) {
            drives.emplace_back(root);
        }
    }
    return drives;
}

std::optional<std::filesystem::path> GetPathFromInstallationDir() {
    std::vector<std::filesystem::path> base_dirs{
        LR"(C:\Program Files\DAUM\PotPlayer)",
        LR"(C:\Program Files (x86)\DAUM\PotPlayer)",
    };
    for (const auto& drive : ExistingDriveRoots()) {
        base_dirs.emplace_back(drive / L"DAUM" / L"PotPlayer");
        base_dirs.emplace_back(drive / L"Program Files" / L"DAUM" / L"PotPlayer");
        base_dirs.emplace_back(drive / L"Program Files (x86)" / L"DAUM" / L"PotPlayer");
    }

    std::error_code ec;
    for (const auto& dir : base_dirs) {
        if (!std::filesystem::exists(dir, ec)) {
            continue;
        }
        const auto translate_dir = dir / L"Extension" / L"Subtitle" / L"Translate";
        if (std::filesystem::exists(translate_dir, ec)) {
            return translate_dir;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ScanDrives() {
    std::error_code ec;
    for (const auto& drive : ExistingDriveRoots()) {
        for (const auto* pf : { L"Program Files", L"Program Files (x86)" }) {
            const auto path = drive / pf / L"DAUM" / L"PotPlayer" / L"Extension" / L"Subtitle" / L"Translate";
            if (std::filesystem::exists(path, ec)) {
                return path;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> AutoDetectDirectory() {
    if (auto shortcut = ScanShortcuts()) {
        return shortcut;
    }
    if (auto install_dir = GetPathFromInstallationDir()) {
        return install_dir;
    }
    return ScanDrives();
}

std::wstring ReadLicenseText() {
    std::string utf8 = LoadResourceTextUtf8(IDR_LICENSE_TEXT);
    if (utf8.empty()) {
        return L"LICENSE file not found.";
    }
    return Utf8ToWide(utf8);
}

std::optional<std::wstring> ReadRegistryString(HKEY root, const std::wstring& subkey, const std::wstring& value_name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, value_name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, value_name.c_str(), nullptr, nullptr, reinterpret_cast<LPBYTE>(value.data()), &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return std::nullopt;
    }
    RegCloseKey(key);
    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::wstring RegKeyName(const std::wstring& install_dir, const std::wstring& context_type) {
    const std::wstring id_base = ToLower(std::filesystem::absolute(install_dir).wstring()) + L"|" + context_type;
    const std::string utf8 = WideToUtf8(id_base);

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_MD5, 0, 0, &hash) ||
        !CryptHashData(hash, reinterpret_cast<const BYTE*>(utf8.data()), static_cast<DWORD>(utf8.size()), 0)) {
        if (hash) {
            CryptDestroyHash(hash);
        }
        if (provider) {
            CryptReleaseContext(provider, 0);
        }
        return L"PotPlayer_ChatGPT_Translate_00000000";
    }

    BYTE digest[16] = {};
    DWORD digest_len = sizeof(digest);
    CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);

    static const wchar_t* hex_chars = L"0123456789abcdef";
    std::wstring hex;
    hex.reserve(8);
    for (int i = 0; i < 4; ++i) {
        hex.push_back(hex_chars[(digest[i] >> 4) & 0x0F]);
        hex.push_back(hex_chars[digest[i] & 0x0F]);
    }
    return L"PotPlayer_ChatGPT_Translate_" + hex;
}

std::optional<RegInfo> FindExistingRegInfo(const std::wstring& install_dir, const std::wstring& context_type) {
    RegInfo info;
    info.key = RegKeyName(install_dir, context_type);
    const std::wstring reg_path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + info.key;
    auto version = ReadRegistryString(HKEY_LOCAL_MACHINE, reg_path, L"DisplayVersion");
    auto uninstall = ReadRegistryString(HKEY_LOCAL_MACHINE, reg_path, L"UninstallString");
    auto context = ReadRegistryString(HKEY_LOCAL_MACHINE, reg_path, L"ContextType");
    if (!version || !uninstall || !context) {
        return std::nullopt;
    }
    info.version = *version;
    info.uninstall = *uninstall;
    info.context = *context;
    return info;
}

bool WriteRegistryString(HKEY key, const std::wstring& name, const std::wstring& value) {
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name.c_str(), 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), size) == ERROR_SUCCESS;
}

bool RegisterSoftware(const std::wstring& display_name,
                      const std::wstring& uninstall_path,
                      const std::wstring& install_dir,
                      const std::wstring& key_name,
                      const std::wstring& version,
                      const std::wstring& context_type) {
    const std::wstring reg_path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + key_name;
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, reg_path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok =
        WriteRegistryString(key, L"DisplayName", display_name) &&
        WriteRegistryString(key, L"UninstallString", uninstall_path) &&
        WriteRegistryString(key, L"InstallLocation", install_dir) &&
        WriteRegistryString(key, L"Publisher", L"Felix3322") &&
        WriteRegistryString(key, L"DisplayIcon", uninstall_path) &&
        WriteRegistryString(key, L"DisplayVersion", version) &&
        WriteRegistryString(key, L"ContextType", context_type);
    RegCloseKey(key);
    return ok;
}

bool GenerateUninstaller(const std::filesystem::path& uninstall_bat_path,
                         const std::vector<std::filesystem::path>& files_to_delete,
                         const std::wstring& reg_key) {
    std::string script;
    script += "@echo off\r\n";
    script += "REM PotPlayer ChatGPT Translate uninstall script\r\n\r\n";
    for (const auto& file : files_to_delete) {
        if (std::filesystem::is_directory(file)) {
            script += "rmdir /s /q \"" + WideToUtf8(file.wstring()) + "\"\r\n";
        } else {
            script += "del \"" + WideToUtf8(file.wstring()) + "\" /f /q\r\n";
        }
    }
    script += "del \"%~f0\" /f /q\r\n";
    script += "reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + WideToUtf8(reg_key) + "\" /f\r\n";
    script += "powershell -NoProfile -Command \"Add-Type -AssemblyName PresentationFramework;[System.Windows.MessageBox]::Show('卸载完成，所有文件均已清理，如果Geek Uninstaller或其他卸载工具提示有残留，请仔细甄别，避免误删PotPlayer本体文件','PotPlayer ChatGPT Translate')\"\r\n";
    script += "\r\nexit\r\n";
    return WriteTextUtf8NoBom(uninstall_bat_path, script);
}

std::pair<bool, std::wstring> VerifyApiSettings(const std::wstring& model, const std::wstring& api_url, const std::wstring& api_key) {
    const std::wstring api_url_for_config = NormalizeApiUrlForConfig(api_url);
    const std::wstring base_url = NormalizeBaseUrlForOpenAI(api_url_for_config);
    std::wstring request_url = base_url;
    while (!request_url.empty() && request_url.back() == L'/') {
        request_url.pop_back();
    }
    request_url += L"/chat/completions";

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    wchar_t host_name[256] = {};
    wchar_t url_path[2048] = {};
    components.lpszHostName = host_name;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host_name));
    components.lpszUrlPath = url_path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(url_path));

    if (!WinHttpCrackUrl(request_url.c_str(), 0, 0, &components)) {
        return { false, GetLastErrorMessage() };
    }

    std::wstring body = L"{\"model\":\"" + EscapeJsonString(model) +
                        L"\",\"messages\":[{\"role\":\"system\",\"content\":\"You are a test assistant.\"},{\"role\":\"user\",\"content\":\"Hello\"}]}";
    const std::string body_utf8 = WideToUtf8(body);

    HINTERNET session = WinHttpOpen(L"PotPlayerChatGPTTranslateInstaller/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return { false, GetLastErrorMessage() };
    }
    WinHttpSetTimeouts(session,
                       static_cast<int>(kVerifyRequestTimeoutSeconds * 1000),
                       static_cast<int>(kVerifyRequestTimeoutSeconds * 1000),
                       static_cast<int>(kVerifyRequestTimeoutSeconds * 1000),
                       static_cast<int>(kVerifyRequestTimeoutSeconds * 1000));

    HINTERNET connect = WinHttpConnect(session, host_name, components.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return { false, GetLastErrorMessage() };
    }

    const DWORD flags = (components.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", url_path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, GetLastErrorMessage() };
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!api_key.empty()) {
        headers += L"Authorization: Bearer " + api_key + L"\r\n";
    }

    const BOOL send_ok = WinHttpSendRequest(
        request,
        headers.c_str(),
        static_cast<DWORD>(headers.size()),
        reinterpret_cast<LPVOID>(const_cast<char*>(body_utf8.data())),
        static_cast<DWORD>(body_utf8.size()),
        static_cast<DWORD>(body_utf8.size()),
        0) &&
        WinHttpReceiveResponse(request, nullptr);

    if (!send_ok) {
        const std::wstring error = GetLastErrorMessage();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return { false, error };
    }

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size, WINHTTP_NO_HEADER_INDEX);

    std::string response_utf8;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            break;
        }
        chunk.resize(read);
        response_utf8 += chunk;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    const bool success = status_code >= 200 && status_code < 300 && response_utf8.find("\"choices\"") != std::string::npos;
    if (success) {
        return { true, L"" };
    }
    if (!response_utf8.empty()) {
        return { false, Utf8ToWide(response_utf8) };
    }
    return { false, L"Empty response" };
}

std::string ReadFileUtf8(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool ApplyPreconfig(const std::filesystem::path& file_path,
                    const std::wstring& api_key,
                    const std::wstring& model,
                    const std::wstring& api_base,
                    int delay_ms,
                    int retry_mode,
                    bool debug_mode,
                    const std::optional<std::wstring>& check_hallucination,
                    const std::optional<std::wstring>& context_subtitle_count,
                    const std::optional<std::wstring>& context_cache_mode,
                    const std::optional<std::wstring>& prompt_cache_retention,
                    const std::optional<std::wstring>& gemini_cached_content,
                    std::optional<bool> small_model,
                    const std::optional<std::string>& token_limits_json) {
    std::string data = ReadFileUtf8(file_path);
    if (data.empty()) {
        return false;
    }

    const std::string normalized_api = WideToUtf8(NormalizeApiUrlForConfig(api_base));
    ReplaceQuotedValue(data, "pre_api_key", WideToUtf8(api_key));
    ReplaceQuotedValue(data, "pre_selected_model", WideToUtf8(model));
    ReplaceQuotedValue(data, "pre_apiUrl", normalized_api);
    ReplaceQuotedValue(data, "pre_delay_ms", std::to_string(delay_ms));
    ReplaceQuotedValue(data, "pre_retry_mode", std::to_string(retry_mode));
    if (small_model.has_value()) {
        ReplaceQuotedValue(data, "pre_small_model", *small_model ? "1" : "0");
    }
    if (check_hallucination.has_value()) {
        ReplaceQuotedValue(data, "pre_check_hallucination",
                           (*check_hallucination == L"1" || *check_hallucination == L"true" || *check_hallucination == L"True" ||
                            *check_hallucination == L"on" || *check_hallucination == L"yes") ? "1" : "0");
    }
    if (context_subtitle_count.has_value()) {
        ReplaceQuotedValue(data, "pre_context_subtitle_count", WideToUtf8(*context_subtitle_count));
    }
    if (context_cache_mode.has_value()) {
        ReplaceQuotedValue(data, "pre_context_cache_mode", WideToUtf8(*context_cache_mode));
    }
    if (prompt_cache_retention.has_value()) {
        ReplaceQuotedValue(data, "pre_prompt_cache_retention", WideToUtf8(*prompt_cache_retention));
    }
    if (gemini_cached_content.has_value()) {
        ReplaceQuotedValue(data, "pre_gemini_cached_content", WideToUtf8(*gemini_cached_content));
    }
    if (token_limits_json.has_value()) {
        ReplaceQuotedValue(data, "pre_model_token_limits_json", WideToUtf8(EscapeForAsString(Utf8ToWide(*token_limits_json))));
    }

    if (debug_mode && data.find("HostOpenConsole();") == std::string::npos) {
        const std::string newline = data.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        std::smatch match;
        const std::regex init_pattern(R"((void\s+OnInitialize\(\)\s*\{\s*\r?\n)([ \t]*))");
        if (std::regex_search(data, match, init_pattern)) {
            const size_t pos = static_cast<size_t>(match.position(1) + match.length(1));
            const std::string indent = match[2].str().empty() ? "    " : match[2].str();
            data.insert(pos, indent + "HostOpenConsole();" + newline);
        } else {
            const size_t comment_end = data.find("*/");
            if (comment_end != std::string::npos) {
                data.insert(comment_end + 2, newline + "HostOpenConsole();" + newline);
            } else {
                data = "HostOpenConsole();" + newline + data;
            }
        }
    }

    return WriteTextUtf8NoBom(file_path, data);
}

std::vector<EmbeddedFileSpec> OfflineFilesForVariant(const std::wstring& variant) {
    if (variant == kVariantWithContext) {
        return {
            { IDR_WITH_CONTEXT_AS, L"SubtitleTranslate - ChatGPT.as", true },
            { IDR_WITH_CONTEXT_ICO, L"SubtitleTranslate - ChatGPT.ico", false },
        };
    }
    return {
        { IDR_WITHOUT_CONTEXT_AS, L"SubtitleTranslate - ChatGPT - Without Context.as", true },
        { IDR_WITHOUT_CONTEXT_ICO, L"SubtitleTranslate - ChatGPT - Without Context.ico", false },
    };
}

void PostInstallLog(HWND progress_hwnd, const std::wstring& message) {
    PostMessageW(progress_hwnd, WM_APP_INSTALL_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
}

void PostInstallDone(HWND progress_hwnd, bool succeeded) {
    PostMessageW(progress_hwnd, WM_APP_INSTALL_DONE, succeeded ? 1 : 0, 0);
}

bool SendPromptAndWait(HWND progress_hwnd, InstallPromptRequest* request) {
    request->event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!request->event_handle) {
        return false;
    }
    PostMessageW(progress_hwnd, WM_APP_INSTALL_PROMPT, 0, reinterpret_cast<LPARAM>(request));
    WaitForSingleObject(request->event_handle, INFINITE);
    CloseHandle(request->event_handle);
    request->event_handle = nullptr;
    return true;
}

int AskFileExists(HWND progress_hwnd, const std::wstring& title, const std::wstring& message) {
    InstallPromptRequest request;
    request.type = InstallPromptType::FileExists;
    request.title = title;
    request.message = message;
    SendPromptAndWait(progress_hwnd, &request);
    return request.choice_result;
}

bool AskYesNo(HWND progress_hwnd, const std::wstring& title, const std::wstring& message) {
    InstallPromptRequest request;
    request.type = InstallPromptType::YesNo;
    request.title = title;
    request.message = message;
    SendPromptAndWait(progress_hwnd, &request);
    return request.bool_result;
}

std::optional<std::wstring> AskText(HWND progress_hwnd, const std::wstring& title, const std::wstring& message) {
    InstallPromptRequest request;
    request.type = InstallPromptType::Text;
    request.title = title;
    request.message = message;
    SendPromptAndWait(progress_hwnd, &request);
    if (!request.accepted) {
        return std::nullopt;
    }
    return request.text_result;
}

InstallStatus InstallVariant(const InstallThreadData& job, const std::wstring& variant) {
    const auto& strings = StringsForLanguage(job.state.language);
    std::vector<std::filesystem::path> files_for_variant;
    bool reg_write = false;
    const std::wstring context_type = variant;
    const std::wstring key_name = RegKeyName(job.state.install_dir, context_type);
    const std::wstring variant_label = variant == kVariantWithContext
                                           ? strings.at(L"with_context_short")
                                           : strings.at(L"without_context_short");
    PostInstallLog(job.progress_hwnd, FormatOneArg(strings.at(L"installing_variant"), variant_label));
    const std::wstring display_suffix = variant == kVariantWithContext
                                            ? S(kLanguageEn, L"with_context_short")
                                            : S(kLanguageEn, L"without_context_short");
    const std::wstring display_name = L"PotPlayer ChatGPT Translate v" + std::wstring(kPluginVersion) + L" [" + display_suffix + L"]";
    const auto reg_info = FindExistingRegInfo(job.state.install_dir, context_type);

    for (const auto& file : OfflineFilesForVariant(variant)) {
        const auto dest_path = std::filesystem::path(job.state.install_dir) / file.dest_name;
        PostInstallLog(job.progress_hwnd, L"Copying " + file.dest_name + L" ...");

        auto resource = LoadResourceBytes(file.resource_id);
        if (!resource) {
            PostInstallLog(job.progress_hwnd, FormatOneArg(strings.at(L"installation_failed"), L"Missing file " + file.dest_name));
            return InstallStatus::Failed;
        }

        auto copy_and_configure = [&](const std::filesystem::path& output_path) -> bool {
            if (!WriteBytesToFile(output_path, resource->data, resource->size)) {
                return false;
            }
            if (file.is_text && EndsWith(ToLower(output_path.filename().wstring()), L".as")) {
                return ApplyPreconfig(
                    output_path,
                    job.state.api_key,
                    job.state.model,
                    job.state.api_base,
                    job.state.delay_ms,
                    job.state.retry_mode,
                    job.state.debug_mode,
                    std::make_optional(job.state.check_hallucination ? std::wstring(L"1") : std::wstring(L"0")),
                    variant == kVariantWithContext ? std::make_optional(std::to_wstring(job.state.context_subtitle_count)) : std::nullopt,
                    variant == kVariantWithContext ? std::make_optional(job.state.context_cache_mode) : std::nullopt,
                    variant == kVariantWithContext ? std::make_optional(job.state.prompt_cache_retention) : std::nullopt,
                    variant == kVariantWithContext ? std::make_optional(job.state.gemini_cached_content) : std::nullopt,
                    std::make_optional(job.state.small_model),
                    std::make_optional(kModelTokenLimitsJson));
            }
            return true;
        };

        std::error_code ec;
        if (std::filesystem::exists(dest_path, ec)) {
            const int choice = AskFileExists(job.progress_hwnd, strings.at(L"app_title"),
                                             FormatOneArg(strings.at(L"file_exists_3choice"), file.dest_name));
            if (choice == 0) {
                PostInstallLog(job.progress_hwnd, MergeBilingual(L"installation_cancelled"));
                return InstallStatus::Cancelled;
            }
            if (choice == 1) {
                if (!copy_and_configure(dest_path)) {
                    PostInstallLog(job.progress_hwnd, FormatOneArg(strings.at(L"installation_failed"), GetLastErrorMessage()));
                    return InstallStatus::Failed;
                }
                PostInstallLog(job.progress_hwnd, L"Installed " + file.dest_name + L" (Overwritten).");
                files_for_variant.push_back(dest_path);
                if (reg_info.has_value()) {
                    if (AskYesNo(job.progress_hwnd, strings.at(L"app_title"), strings.at(L"ask_reg_upgrade"))) {
                        reg_write = true;
                    }
                } else {
                    if (AskYesNo(job.progress_hwnd, strings.at(L"app_title"), strings.at(L"ask_reg_write"))) {
                        reg_write = true;
                    }
                }
            } else if (choice == 2) {
                for (;;) {
                    auto new_name = AskText(job.progress_hwnd, strings.at(L"app_title"), strings.at(L"rename"));
                    if (!new_name.has_value()) {
                        PostInstallLog(job.progress_hwnd, MergeBilingual(L"installation_cancelled"));
                        return InstallStatus::Cancelled;
                    }
                    std::wstring trimmed = Trim(*new_name);
                    if (trimmed.empty()) {
                        continue;
                    }
                    std::filesystem::path renamed = std::filesystem::path(trimmed);
                    if (!renamed.has_extension()) {
                        renamed += dest_path.extension();
                    }
                    const auto new_dest_path = std::filesystem::path(job.state.install_dir) / renamed;
                    if (std::filesystem::exists(new_dest_path, ec)) {
                        AskFileExists(job.progress_hwnd, strings.at(L"app_title"),
                                      FormatOneArg(strings.at(L"file_exists_3choice"), renamed.wstring()));
                        continue;
                    }
                    if (!copy_and_configure(new_dest_path)) {
                        PostInstallLog(job.progress_hwnd, FormatOneArg(strings.at(L"installation_failed"), GetLastErrorMessage()));
                        return InstallStatus::Failed;
                    }
                    PostInstallLog(job.progress_hwnd, L"Installed " + renamed.wstring() + L".");
                    files_for_variant.push_back(new_dest_path);
                    if (AskYesNo(job.progress_hwnd, strings.at(L"app_title"), strings.at(L"ask_reg_new"))) {
                        reg_write = true;
                    }
                    break;
                }
            }
        } else {
            if (!copy_and_configure(dest_path)) {
                PostInstallLog(job.progress_hwnd, FormatOneArg(strings.at(L"installation_failed"), GetLastErrorMessage()));
                return InstallStatus::Failed;
            }
            PostInstallLog(job.progress_hwnd, L"Installed " + file.dest_name + L".");
            files_for_variant.push_back(dest_path);
            if (AskYesNo(job.progress_hwnd, strings.at(L"app_title"), strings.at(L"ask_reg_new"))) {
                reg_write = true;
            }
        }
    }

    if (reg_write) {
        const auto tools_dir = std::filesystem::path(job.state.install_dir) / L"tools";
        EnsureDirExists(tools_dir);
        const auto uninstaller_path = tools_dir / (L"uninstaller_" + key_name + L".bat");
        auto files_to_delete = files_for_variant;
        files_to_delete.push_back(uninstaller_path);
        if (!GenerateUninstaller(uninstaller_path, files_to_delete, key_name) ||
            !RegisterSoftware(display_name, uninstaller_path.wstring(), job.state.install_dir, key_name,
                              kPluginVersion, context_type)) {
            PostInstallLog(job.progress_hwnd, FormatOneArg(strings.at(L"installation_failed"), GetLastErrorMessage()));
            return InstallStatus::Failed;
        }
    }

    return InstallStatus::Success;
}

DWORD WINAPI InstallThreadProc(LPVOID param) {
    std::unique_ptr<InstallThreadData> job(reinterpret_cast<InstallThreadData*>(param));
    const auto& strings = StringsForLanguage(job->state.language);
    if (!EnsureDirExists(job->state.install_dir)) {
        PostInstallLog(job->progress_hwnd, FormatOneArg(strings.at(L"installation_failed"), GetLastErrorMessage()));
        PostInstallDone(job->progress_hwnd, false);
        return 0;
    }

    if (job->state.versions.empty()) {
        PostInstallLog(job->progress_hwnd, FormatOneArg(strings.at(L"installation_failed"), L"No variant selected"));
        PostInstallDone(job->progress_hwnd, false);
        return 0;
    }

    for (const auto& variant : job->state.versions) {
        const InstallStatus status = InstallVariant(*job, variant);
        if (status != InstallStatus::Success) {
            PostInstallDone(job->progress_hwnd, false);
            return 0;
        }
    }

    PostInstallLog(job->progress_hwnd, S(kLanguageEn, L"installation_complete") + L"\n" + S(kLanguageZh, L"installation_complete"));
    PostInstallLog(job->progress_hwnd, L"DONE");
    PostInstallDone(job->progress_hwnd, true);
    return 0;
}

} // namespace

namespace {

void SetWizardCommonTexts(HWND wizard_hwnd, const std::wstring& language);
void SetWizardButtons(HWND page_hwnd, DWORD buttons, bool enable_cancel = true);
void AppendEditText(HWND edit_hwnd, const std::wstring& message);
HRESULT BrowseForDirectory(HWND owner, const std::wstring& current_dir, std::wstring& selected_dir);
void OpenExternalLink(const std::wstring& url);
INT_PTR CALLBACK InputDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
std::optional<std::wstring> ShowTextInputLoop(HWND owner, const std::wstring& title, const std::wstring& prompt);
void HandlePromptRequest(HWND page_hwnd, InstallPromptRequest* request);
void InitPageDialog(HWND hwnd, LPARAM lParam);
void MaybeHandleLinkClick(HWND hwnd, LPARAM lParam);
void SetConfigStatus(HWND hwnd, const std::wstring& message);
void ApplyConfigSelection(HWND hwnd, bool initializing);
void InitializeConfigPage(HWND hwnd);
bool RunConfigVerification(HWND hwnd, bool allow_status_update);

INT_PTR CALLBACK LanguagePageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK WelcomePageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK LicensePageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK DirectoryPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK VersionPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK ConfigPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK DelayPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK RetryPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK HallucinationPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK ContextPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK DebugPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK ProgressPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK FinishPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
int CALLBACK PropSheetCallback(HWND hwnd, UINT msg, LPARAM lParam);
PROPSHEETPAGEW MakePage(HINSTANCE instance, int template_id, DLGPROC proc, LPARAM lparam);

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LINK_CLASS | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WizardContext ctx;
    ctx.instance = instance;
    ctx.bg_brush = CreateSolidBrush(kBgColor);
    ctx.edit_brush = CreateSolidBrush(kEditColor);
    g_ctx = &ctx;

    if (!IsAdmin()) {
        ShowSimpleMessage(nullptr, S(kLanguageEn, L"app_title"), MergeBilingual(L"admin_required"), MB_OK | MB_ICONWARNING);
        RestartAsAdmin();
        DeleteObject(ctx.bg_brush);
        DeleteObject(ctx.edit_brush);
        CoUninitialize();
        return 0;
    }

    PROPSHEETPAGEW pages[] = {
        MakePage(instance, IDD_PAGE_LANGUAGE, LanguagePageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_WELCOME, WelcomePageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_LICENSE, LicensePageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_DIRECTORY, DirectoryPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_VERSION, VersionPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_CONFIG, ConfigPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_DELAY, DelayPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_RETRY, RetryPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_HALLUCINATION, HallucinationPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_CONTEXT, ContextPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_DEBUG, DebugPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_PROGRESS, ProgressPageProc, reinterpret_cast<LPARAM>(&ctx)),
        MakePage(instance, IDD_PAGE_FINISH, FinishPageProc, reinterpret_cast<LPARAM>(&ctx)),
    };

    PROPSHEETHEADERW header{};
    header.dwSize = sizeof(header);
    header.dwFlags = PSH_WIZARD | PSH_PROPSHEETPAGE | PSH_USECALLBACK | PSH_USEICONID;
    header.hwndParent = nullptr;
    header.hInstance = instance;
    header.pszCaption = S(ctx, L"app_title").c_str();
    header.pszIcon = MAKEINTRESOURCEW(IDI_INSTALLER_ICON);
    header.nPages = static_cast<UINT>(sizeof(pages) / sizeof(pages[0]));
    header.ppsp = pages;
    header.pfnCallback = PropSheetCallback;

    PropertySheetW(&header);

    if (ctx.progress_ui.thread_handle) {
        WaitForSingleObject(ctx.progress_ui.thread_handle, INFINITE);
        CloseHandle(ctx.progress_ui.thread_handle);
    }
    DeleteObject(ctx.bg_brush);
    DeleteObject(ctx.edit_brush);
    CoUninitialize();
    return 0;
}

namespace {

INT_PTR HandleDialogColors(HWND hwnd, UINT message, WPARAM wParam) {
    auto* ctx = GetDialogState<WizardContext>(hwnd);
    return ctx ? HandleDarkColorMessage(wParam, message, ctx) : FALSE;
}

INT_PTR CALLBACK LanguagePageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        CheckRadioButton(hwnd, IDC_LANG_EN, IDC_LANG_ZH, IDC_LANG_EN);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_LANG_TITLE, S(kLanguageEn, L"choose_language") + L" / " + S(kLanguageZh, L"choose_language"));
            SetText(hwnd, IDC_LANG_EN, S(kLanguageEn, L"language_english"));
            SetText(hwnd, IDC_LANG_ZH, S(kLanguageZh, L"language_chinese"));
            CheckRadioButton(hwnd, IDC_LANG_EN, IDC_LANG_ZH, ctx->state.language == kLanguageZh ? IDC_LANG_ZH : IDC_LANG_EN);
            SetWizardButtons(hwnd, PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            ctx->state.language = IsDlgButtonChecked(hwnd, IDC_LANG_ZH) == BST_CHECKED ? kLanguageZh : kLanguageEn;
            SetWizardCommonTexts(GetParent(hwnd), ctx->state.language);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK WelcomePageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->idFrom == IDC_AUTHOR_LINK) {
            MaybeHandleLinkClick(hwnd, lParam);
            return TRUE;
        }
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_WELCOME_TITLE, S(*ctx, L"welcome_title"));
            SetText(hwnd, IDC_WELCOME_TEXT, ToWindowsNewlines(S(*ctx, L"welcome_message")));
            SetText(hwnd, IDC_AUTHOR_LINK,
                    L"<a href=\"https://github.com/Felix3322/PotPlayer_ChatGPT_Translate\">" + S(*ctx, L"author_info") + L"</a>");
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK LicensePageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_LICENSE_TITLE, S(*ctx, L"license_title"));
            SetText(hwnd, IDC_LICENSE_INTRO, S(*ctx, L"license_intro"));
            SetText(hwnd, IDC_LICENSE_TEXT, ToWindowsNewlines(ReadLicenseText()));
            SetText(hwnd, IDC_LICENSE_AGREE, S(*ctx, L"license_agree"));
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            if (IsDlgButtonChecked(hwnd, IDC_LICENSE_AGREE) != BST_CHECKED) {
                ShowSimpleMessage(hwnd, S(*ctx, L"app_title"), S(*ctx, L"license_reject"), MB_OK | MB_ICONWARNING);
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, -1);
                return TRUE;
            }
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK DirectoryPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_DIRECTORY_BROWSE && HIWORD(wParam) == BN_CLICKED) {
            std::wstring selected;
            if (SUCCEEDED(BrowseForDirectory(hwnd, GetText(hwnd, IDC_DIRECTORY_EDIT), selected))) {
                SetText(hwnd, IDC_DIRECTORY_EDIT, selected);
            }
            return TRUE;
        }
        break;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_DIRECTORY_TITLE, S(*ctx, L"select_install_dir_title"));
            SetText(hwnd, IDC_DIRECTORY_INFO, ToWindowsNewlines(S(*ctx, L"select_install_dir_explain")));
            SetWindowTextW(GetDlgItem(hwnd, IDC_DIRECTORY_BROWSE), S(*ctx, L"browse").c_str());
            SetText(hwnd, IDC_DIRECTORY_EDIT, ctx->state.install_dir);
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            if (ctx->state.install_dir.empty()) {
                if (auto detected = AutoDetectDirectory()) {
                    const auto question = FormatOneArg(S(*ctx, L"confirm_path"), detected->wstring());
                    if (MessageBoxW(hwnd, ToWindowsNewlines(question).c_str(), S(*ctx, L"app_title").c_str(),
                                    MB_ICONQUESTION | MB_YESNO) == IDYES) {
                        SetText(hwnd, IDC_DIRECTORY_EDIT, detected->wstring());
                    }
                } else {
                    ShowSimpleMessage(hwnd, S(*ctx, L"app_title"), S(*ctx, L"not_detected"));
                }
            }
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            const std::wstring value = Trim(GetText(hwnd, IDC_DIRECTORY_EDIT));
            if (value.empty()) {
                ShowSimpleMessage(hwnd, S(*ctx, L"app_title"), S(*ctx, L"select_directory"), MB_OK | MB_ICONWARNING);
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, -1);
                return TRUE;
            }
            ctx->state.install_dir = value;
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK VersionPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_VERSION_TITLE, S(*ctx, L"choose_version_title"));
            SetText(hwnd, IDC_VERSION_PROMPT, S(*ctx, L"choose_version"));
            SetText(hwnd, IDC_WITH_CONTEXT, S(*ctx, L"with_context"));
            SetText(hwnd, IDC_WITH_CONTEXT_DESC, ToWindowsNewlines(S(*ctx, L"with_context_description")));
            SetText(hwnd, IDC_WITHOUT_CONTEXT, S(*ctx, L"without_context"));
            SetText(hwnd, IDC_WITHOUT_CONTEXT_DESC, ToWindowsNewlines(S(*ctx, L"without_context_description")));
            SetText(hwnd, IDC_VERSION_EXPLAIN, ToWindowsNewlines(S(*ctx, L"version_explain")));
            CheckDlgButton(hwnd, IDC_WITH_CONTEXT,
                           std::find(ctx->state.versions.begin(), ctx->state.versions.end(), kVariantWithContext) != ctx->state.versions.end()
                               ? BST_CHECKED
                               : (ctx->state.versions.empty() ? BST_CHECKED : BST_UNCHECKED));
            CheckDlgButton(hwnd, IDC_WITHOUT_CONTEXT,
                           std::find(ctx->state.versions.begin(), ctx->state.versions.end(), kVariantWithoutContext) != ctx->state.versions.end()
                               ? BST_CHECKED
                               : BST_UNCHECKED);
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            std::vector<std::wstring> selections;
            if (IsDlgButtonChecked(hwnd, IDC_WITH_CONTEXT) == BST_CHECKED) {
                selections.push_back(kVariantWithContext);
            }
            if (IsDlgButtonChecked(hwnd, IDC_WITHOUT_CONTEXT) == BST_CHECKED) {
                selections.push_back(kVariantWithoutContext);
            }
            if (selections.empty()) {
                ShowSimpleMessage(hwnd, S(*ctx, L"app_title"), S(*ctx, L"version_select_warning"), MB_OK | MB_ICONWARNING);
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, -1);
                return TRUE;
            }
            ctx->state.versions = selections;
            ctx->state.has_context_variant = std::find(selections.begin(), selections.end(), kVariantWithContext) != selections.end();
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK ConfigPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CONFIG_MODEL_PRESET && HIWORD(wParam) == CBN_SELCHANGE) {
            ApplyConfigSelection(hwnd, false);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CONFIG_PURCHASE && HIWORD(wParam) == BN_CLICKED) {
            auto* ctx = GetDialogState<WizardContext>(hwnd);
            if (!ctx->config_ui.purchase_link.empty()) {
                OpenExternalLink(ctx->config_ui.purchase_link);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CONFIG_VERIFY && HIWORD(wParam) == BN_CLICKED) {
            RunConfigVerification(hwnd, true);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CONFIG_SKIP && HIWORD(wParam) == BN_CLICKED) {
            auto* ctx = GetDialogState<WizardContext>(hwnd);
            ctx->config_ui.skip_requested = true;
            PropSheet_PressButton(GetParent(hwnd), PSBTN_NEXT);
            return TRUE;
        }
        break;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            InitializeConfigPage(hwnd);
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            ctx->state.model = Trim(GetText(hwnd, IDC_CONFIG_MODEL_EDIT));
            ctx->state.api_base = NormalizeApiUrlForConfig(Trim(GetText(hwnd, IDC_CONFIG_API_EDIT)));
            ctx->state.api_key = Trim(GetText(hwnd, IDC_CONFIG_KEY_EDIT));
            ctx->state.small_model = IsDlgButtonChecked(hwnd, IDC_CONFIG_SMALL_MODEL) == BST_CHECKED;
            if (ctx->config_ui.skip_requested) {
                ctx->config_ui.skip_requested = false;
                return TRUE;
            }
            if (!RunConfigVerification(hwnd, true)) {
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, -1);
                return TRUE;
            }
            if (ctx->state.api_key.empty() && ctx->config_ui.empty_key_verified) {
                ctx->state.api_key = L"nullkey";
            }
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK DelayPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        SendMessageW(GetDlgItem(hwnd, IDC_DELAY_SPIN), UDM_SETRANGE32, 0, 60000);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->idFrom == IDC_DELAY_INTRO) {
            MaybeHandleLinkClick(hwnd, lParam);
            return TRUE;
        }
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_DELAY_TITLE, S(*ctx, L"delay_title"));
            SetText(hwnd, IDC_DELAY_INTRO, ToWindowsNewlines(S(*ctx, L"delay_intro")));
            SetText(hwnd, IDC_DELAY_LABEL, S(*ctx, L"delay_label"));
            SetIntText(hwnd, IDC_DELAY_EDIT, ctx->state.delay_ms);
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            ctx->state.delay_ms = GetIntText(hwnd, IDC_DELAY_EDIT, ctx->state.delay_ms);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK RetryPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            const auto& s = StringsForLanguage(ctx->state.language);
            SetText(hwnd, IDC_RETRY_TITLE, s.at(L"retry_title"));
            SetText(hwnd, IDC_RETRY_INTRO, ToWindowsNewlines(s.at(L"retry_intro")));
            HWND combo = GetDlgItem(hwnd, IDC_RETRY_COMBO);
            ComboBox_ResetContent(combo);
            ComboBox_AddString(combo, s.at(L"retry_off").c_str());
            ComboBox_AddString(combo, s.at(L"retry_once").c_str());
            ComboBox_AddString(combo, s.at(L"retry_until").c_str());
            ComboBox_AddString(combo, s.at(L"retry_until_delay").c_str());
            ComboBox_SetCurSel(combo, ctx->state.retry_mode);
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            ctx->state.retry_mode = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_RETRY_COMBO));
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK HallucinationPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_HALLUCINATION_TITLE, S(*ctx, L"hallucination_title"));
            SetText(hwnd, IDC_HALLUCINATION_INTRO, ToWindowsNewlines(S(*ctx, L"hallucination_intro")));
            SetText(hwnd, IDC_HALLUCINATION_CHECK, S(*ctx, L"hallucination_label"));
            CheckDlgButton(hwnd, IDC_HALLUCINATION_CHECK, ctx->state.check_hallucination ? BST_CHECKED : BST_UNCHECKED);
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            ctx->state.check_hallucination = IsDlgButtonChecked(hwnd, IDC_HALLUCINATION_CHECK) == BST_CHECKED;
            if (!ctx->state.has_context_variant) {
                SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, IDD_PAGE_DEBUG);
            }
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK ContextPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        InitPageDialog(hwnd, lParam);
        SendMessageW(GetDlgItem(hwnd, IDC_CONTEXT_COUNT_SPIN), UDM_SETRANGE32, 0, 20);
        UDACCEL accel{ 0, 1 };
        SendMessageW(GetDlgItem(hwnd, IDC_CONTEXT_COUNT_SPIN), UDM_SETACCEL, 1, reinterpret_cast<LPARAM>(&accel));
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            const auto& s = StringsForLanguage(ctx->state.language);
            SetText(hwnd, IDC_CONTEXT_TITLE, s.at(L"context_title"));
            SetText(hwnd, IDC_CONTEXT_INTRO, ToWindowsNewlines(s.at(L"context_intro")));
            SetText(hwnd, IDC_CONTEXT_COUNT_LABEL, s.at(L"context_count_label"));
            SetText(hwnd, IDC_CONTEXT_COUNT_HINT, ToWindowsNewlines(s.at(L"context_count_hint")));
            SetText(hwnd, IDC_CONTEXT_CACHE_LABEL, s.at(L"context_cache_label"));
            SetText(hwnd, IDC_CONTEXT_CACHE_HINT, ToWindowsNewlines(s.at(L"context_cache_hint")));
            SetIntText(hwnd, IDC_CONTEXT_COUNT_EDIT, ctx->state.context_subtitle_count);

            HWND cache_combo = GetDlgItem(hwnd, IDC_CONTEXT_CACHE_COMBO);
            ComboBox_ResetContent(cache_combo);
            ComboBox_AddString(cache_combo, s.at(L"context_cache_auto").c_str());
            ComboBox_AddString(cache_combo, s.at(L"context_cache_off").c_str());
            ComboBox_SetCurSel(cache_combo, ctx->state.context_cache_mode == L"auto" ? 0 : 1);

            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            ctx->state.context_subtitle_count = GetIntText(hwnd, IDC_CONTEXT_COUNT_EDIT, ctx->state.context_subtitle_count);
            ctx->state.context_cache_mode =
                ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CONTEXT_CACHE_COMBO)) == 0 ? L"auto" : L"off";
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK DebugPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_DEBUG_TITLE, S(*ctx, L"debug_title"));
            SetText(hwnd, IDC_DEBUG_CHECK, S(*ctx, L"debug_label"));
            CheckDlgButton(hwnd, IDC_DEBUG_CHECK, ctx->state.debug_mode ? BST_CHECKED : BST_UNCHECKED);
            SetWizardButtons(hwnd, PSWIZB_BACK | PSWIZB_NEXT | PSWIZB_CANCEL);
            return TRUE;
        }
        if (hdr->code == PSN_WIZNEXT) {
            ctx->state.debug_mode = IsDlgButtonChecked(hwnd, IDC_DEBUG_CHECK) == BST_CHECKED;
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK ProgressPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        GetDialogState<WizardContext>(hwnd)->progress_ui.hwnd = hwnd;
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_APP_INSTALL_LOG: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (text) {
            AppendEditText(GetDlgItem(hwnd, IDC_PROGRESS_LOG), *text);
        }
        return TRUE;
    }
    case WM_APP_INSTALL_DONE: {
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        ctx->progress_ui.completed = true;
        ctx->progress_ui.succeeded = wParam == 1;
        SetWizardButtons(hwnd, ctx->progress_ui.succeeded ? 0 : (PSWIZB_BACK | PSWIZB_CANCEL), true);
        if (ctx->progress_ui.succeeded) {
            PropSheet_PressButton(GetParent(hwnd), PSBTN_NEXT);
        }
        return TRUE;
    }
    case WM_APP_INSTALL_PROMPT:
        HandlePromptRequest(hwnd, reinterpret_cast<InstallPromptRequest*>(lParam));
        return TRUE;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_PROGRESS_TITLE, S(*ctx, L"install_progress_title"));
            SetText(hwnd, IDC_PROGRESS_HEADER, S(*ctx, L"install_progress"));
            SetDlgItemTextW(hwnd, IDC_PROGRESS_LOG, L"");
            SetWizardButtons(hwnd, 0, false);
            if (!ctx->progress_ui.started) {
                ctx->progress_ui.started = true;
                auto data = std::make_unique<InstallThreadData>();
                data->state = ctx->state;
                data->progress_hwnd = hwnd;
                ctx->progress_ui.thread_handle = CreateThread(nullptr, 0, InstallThreadProc, data.release(), 0, nullptr);
            }
            return TRUE;
        }
        if (hdr->code == PSN_QUERYCANCEL && !ctx->progress_ui.completed) {
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

INT_PTR CALLBACK FinishPageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        InitPageDialog(hwnd, lParam);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDialogColors(hwnd, message, wParam);
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        auto* ctx = GetDialogState<WizardContext>(hwnd);
        if (hdr->code == PSN_SETACTIVE) {
            SetText(hwnd, IDC_FINISH_TITLE, S(*ctx, L"finish_title"));
            SetText(hwnd, IDC_FINISH_LABEL, ToWindowsNewlines(S(*ctx, L"installation_complete")));
            SetWizardButtons(hwnd, PSWIZB_FINISH | PSWIZB_BACK, true);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

int CALLBACK PropSheetCallback(HWND hwnd, UINT msg, LPARAM) {
    if (msg == PSCB_INITIALIZED && g_ctx) {
        EnableDarkTitleBar(hwnd);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(g_ctx->instance, MAKEINTRESOURCEW(IDI_INSTALLER_ICON))));
        SetWizardCommonTexts(hwnd, g_ctx->state.language);
    }
    return 0;
}

PROPSHEETPAGEW MakePage(HINSTANCE instance, int template_id, DLGPROC proc, LPARAM lparam) {
    PROPSHEETPAGEW page{};
    page.dwSize = sizeof(page);
    page.dwFlags = PSP_DEFAULT;
    page.hInstance = instance;
    page.pszTemplate = MAKEINTRESOURCEW(template_id);
    page.pfnDlgProc = proc;
    page.lParam = lparam;
    return page;
}

} // namespace

namespace {

void SetWizardCommonTexts(HWND wizard_hwnd, const std::wstring& language) {
    SetWindowTextW(wizard_hwnd, S(language, L"app_title").c_str());
    if (HWND back = GetDlgItem(wizard_hwnd, ID_WIZBACK)) {
        SetWindowTextW(back, S(language, L"back").c_str());
    }
    if (HWND next = GetDlgItem(wizard_hwnd, ID_WIZNEXT)) {
        SetWindowTextW(next, S(language, L"next").c_str());
    }
    if (HWND finish = GetDlgItem(wizard_hwnd, ID_WIZFINISH)) {
        SetWindowTextW(finish, S(language, L"finish").c_str());
    }
    if (HWND cancel = GetDlgItem(wizard_hwnd, IDCANCEL)) {
        SetWindowTextW(cancel, S(language, L"cancel").c_str());
    }
}

void SetWizardButtons(HWND page_hwnd, DWORD buttons, bool enable_cancel) {
    HWND wizard_hwnd = GetParent(page_hwnd);
    PropSheet_SetWizButtons(wizard_hwnd, buttons);
    SetWizardCommonTexts(wizard_hwnd, g_ctx->state.language);
    if (HWND cancel = GetDlgItem(wizard_hwnd, IDCANCEL)) {
        EnableWindow(cancel, enable_cancel ? TRUE : FALSE);
    }
}

void AppendEditText(HWND edit_hwnd, const std::wstring& message) {
    const std::wstring line = ToWindowsNewlines(message) + L"\r\n";
    SendMessageW(edit_hwnd, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(edit_hwnd, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

HRESULT BrowseForDirectory(HWND owner, const std::wstring& current_dir, std::wstring& selected_dir) {
    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return hr;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(S(g_ctx->state.language, L"select_directory").c_str());
    if (!current_dir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(current_dir.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    hr = dialog->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* result = nullptr;
        hr = dialog->GetResult(&result);
        if (SUCCEEDED(hr) && result) {
            PWSTR path = nullptr;
            hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(hr) && path) {
                selected_dir = path;
                CoTaskMemFree(path);
            }
            result->Release();
        }
    }

    dialog->Release();
    return hr;
}

void OpenExternalLink(const std::wstring& url) {
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

INT_PTR CALLBACK InputDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* state = reinterpret_cast<InputDialogState*>(lParam);
        SetDialogState(hwnd, state);
        EnableDarkTitleBar(hwnd);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(g_ctx->instance, MAKEINTRESOURCEW(IDI_INSTALLER_ICON))));
        SetWindowTextW(hwnd, state->title.c_str());
        SetText(hwnd, IDC_INPUT_PROMPT, ToWindowsNewlines(state->prompt));
        SetDlgItemTextW(hwnd, IDC_INPUT_EDIT, state->value.c_str());
        SetWindowTextW(GetDlgItem(hwnd, IDCANCEL), S(g_ctx->state.language, L"cancel").c_str());
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleDarkColorMessage(wParam, message, g_ctx);
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* state = GetDialogState<InputDialogState>(hwnd);
            state->value = Trim(GetText(hwnd, IDC_INPUT_EDIT));
            state->accepted = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

std::optional<std::wstring> ShowTextInputLoop(HWND owner, const std::wstring& title, const std::wstring& prompt) {
    for (;;) {
        InputDialogState state;
        state.ctx = g_ctx;
        state.title = title;
        state.prompt = prompt;
        const INT_PTR result = DialogBoxParamW(g_ctx->instance, MAKEINTRESOURCEW(IDD_INPUT_TEXT), owner, InputDialogProc,
                                               reinterpret_cast<LPARAM>(&state));
        if (result != IDOK || !state.accepted) {
            return std::nullopt;
        }
        if (!state.value.empty()) {
            return state.value;
        }
        ShowSimpleMessage(owner, title, S(g_ctx->state.language, L"rename"), MB_OK | MB_ICONWARNING);
    }
}

void HandlePromptRequest(HWND page_hwnd, InstallPromptRequest* request) {
    if (!request) {
        return;
    }

    switch (request->type) {
    case InstallPromptType::FileExists: {
        TASKDIALOG_BUTTON buttons[] = {
            { 1001, S(g_ctx->state.language, L"overwrite").c_str() },
            { 1002, S(g_ctx->state.language, L"rename").c_str() },
            { IDCANCEL, S(g_ctx->state.language, L"cancel").c_str() },
        };
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hwndParent = page_hwnd;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        config.pszWindowTitle = request->title.c_str();
        config.pszContent = ToWindowsNewlines(request->message).c_str();
        config.pButtons = buttons;
        config.cButtons = ARRAYSIZE(buttons);
        config.nDefaultButton = 1001;
        int button = IDCANCEL;
        TaskDialogIndirect(&config, &button, nullptr, nullptr);
        request->choice_result = button == 1001 ? 1 : (button == 1002 ? 2 : 0);
        break;
    }
    case InstallPromptType::YesNo:
        request->bool_result = MessageBoxW(page_hwnd, ToWindowsNewlines(request->message).c_str(), request->title.c_str(),
                                           MB_ICONQUESTION | MB_YESNO) == IDYES;
        break;
    case InstallPromptType::Text: {
        auto text = ShowTextInputLoop(page_hwnd, request->title, request->message);
        if (text.has_value()) {
            request->accepted = true;
            request->text_result = *text;
        }
        break;
    }
    }

    if (request->event_handle) {
        SetEvent(request->event_handle);
    }
}

void InitPageDialog(HWND hwnd, LPARAM lParam) {
    auto* psp = reinterpret_cast<LPPROPSHEETPAGEW>(lParam);
    auto* ctx = reinterpret_cast<WizardContext*>(psp->lParam);
    SetDialogState(hwnd, ctx);
    EnableDarkTitleBar(hwnd);
}

void MaybeHandleLinkClick(HWND hwnd, LPARAM lParam) {
    auto* header = reinterpret_cast<NMHDR*>(lParam);
    if (!header || (header->code != NM_CLICK && header->code != NM_RETURN)) {
        return;
    }
    auto* link = reinterpret_cast<PNMLINK>(lParam);
    if (link->item.szUrl[0] != L'\0') {
        OpenExternalLink(link->item.szUrl);
    }
}

void SetConfigStatus(HWND hwnd, const std::wstring& message) {
    SetText(hwnd, IDC_CONFIG_STATUS, ToWindowsNewlines(message));
}

void ApplyConfigSelection(HWND hwnd, bool initializing) {
    auto* ctx = GetDialogState<WizardContext>(hwnd);
    const int selected = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CONFIG_MODEL_PRESET));
    if (selected < 0 || selected >= static_cast<int>(ctx->config_ui.presets.size())) {
        EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_MODEL_EDIT), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_API_EDIT), TRUE);
        if (!initializing) {
            SetText(hwnd, IDC_CONFIG_MODEL_EDIT, L"");
            SetText(hwnd, IDC_CONFIG_API_EDIT, L"");
        }
        ctx->config_ui.purchase_link.clear();
        if (!initializing) {
            SetConfigStatus(hwnd, L"");
        }
        return;
    }

    const auto& preset = ctx->config_ui.presets[selected];
    const auto* provider = FindProvider(preset.provider_key);
    if (!provider) {
        return;
    }
    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_MODEL_EDIT), provider->allow_custom_model ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_API_EDIT), TRUE);
    SetText(hwnd, IDC_CONFIG_MODEL_EDIT, provider->model);
    SetText(hwnd, IDC_CONFIG_API_EDIT, NormalizeApiUrlForConfig(provider->api_base));
    ctx->config_ui.purchase_link = provider->purchase_page;
    if (!initializing) {
        SetConfigStatus(hwnd, L"");
    }
}

void InitializeConfigPage(HWND hwnd) {
    auto* ctx = GetDialogState<WizardContext>(hwnd);
    const auto& s = StringsForLanguage(ctx->state.language);
    ctx->config_ui.presets = BuildProviderPresetEntries();

    SetText(hwnd, IDC_CONFIG_TITLE, s.at(L"config_title"));
    SetText(hwnd, IDC_CONFIG_INTRO, ToWindowsNewlines(s.at(L"config_intro")));
    SetText(hwnd, IDC_CONFIG_PRESET_LABEL, s.at(L"config_model_preset"));
    SetText(hwnd, IDC_CONFIG_MODEL_LABEL, s.at(L"config_model"));
    SetText(hwnd, IDC_CONFIG_API_LABEL, s.at(L"config_api"));
    SetText(hwnd, IDC_CONFIG_KEY_LABEL, s.at(L"config_key"));
    SetText(hwnd, IDC_CONFIG_SMALL_MODEL, s.at(L"smallmodel_label"));
    SetWindowTextW(GetDlgItem(hwnd, IDC_CONFIG_PURCHASE), s.at(L"purchase_button").c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_CONFIG_VERIFY), s.at(L"verify").c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_CONFIG_SKIP), s.at(L"skip").c_str());
    SendMessageW(GetDlgItem(hwnd, IDC_CONFIG_KEY_EDIT), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(s.at(L"config_key_placeholder").c_str()));

    HWND combo = GetDlgItem(hwnd, IDC_CONFIG_MODEL_PRESET);
    ComboBox_ResetContent(combo);
    for (const auto& preset : ctx->config_ui.presets) {
        ComboBox_AddString(combo, preset.label.c_str());
    }
    ComboBox_AddString(combo, L"Custom...");

    const std::wstring normalized_api = NormalizeApiUrlForConfig(ctx->state.api_base);
    int target_index = -1;
    for (int i = 0; i < static_cast<int>(ctx->config_ui.presets.size()); ++i) {
        const auto* provider = FindProvider(ctx->config_ui.presets[i].provider_key);
        if (!provider) {
            continue;
        }
        const std::wstring provider_api = NormalizeApiUrlForConfig(provider->api_base);
        if (provider->model == ctx->state.model && provider_api == normalized_api) {
            target_index = i;
            break;
        }
        if (provider->allow_custom_model && provider_api == normalized_api) {
            target_index = i;
        }
    }

    if (target_index >= 0) {
        ComboBox_SetCurSel(combo, target_index);
        ApplyConfigSelection(hwnd, true);
        SetText(hwnd, IDC_CONFIG_MODEL_EDIT, ctx->state.model);
        SetText(hwnd, IDC_CONFIG_API_EDIT, normalized_api);
    } else {
        ComboBox_SetCurSel(combo, static_cast<int>(ctx->config_ui.presets.size()));
        EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_MODEL_EDIT), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_API_EDIT), TRUE);
        SetText(hwnd, IDC_CONFIG_MODEL_EDIT, ctx->state.model);
        SetText(hwnd, IDC_CONFIG_API_EDIT, normalized_api);
        ctx->config_ui.purchase_link.clear();
    }

    SetText(hwnd, IDC_CONFIG_KEY_EDIT, ctx->state.api_key);
    CheckDlgButton(hwnd, IDC_CONFIG_SMALL_MODEL, ctx->state.small_model ? BST_CHECKED : BST_UNCHECKED);
    ctx->config_ui.skip_requested = false;
    ctx->config_ui.empty_key_verified = false;
    SetConfigStatus(hwnd, L"");
}

bool RunConfigVerification(HWND hwnd, bool allow_status_update = true) {
    auto* ctx = GetDialogState<WizardContext>(hwnd);
    auto& s = StringsForLanguage(ctx->state.language);
    const std::wstring model = Trim(GetText(hwnd, IDC_CONFIG_MODEL_EDIT));
    const std::wstring api_url = Trim(GetText(hwnd, IDC_CONFIG_API_EDIT));
    const std::wstring api_key = Trim(GetText(hwnd, IDC_CONFIG_KEY_EDIT));

    if (allow_status_update) {
        SetConfigStatus(hwnd, s.at(L"verifying"));
        UpdateWindow(GetDlgItem(hwnd, IDC_CONFIG_STATUS));
    }
    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_PURCHASE), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_VERIFY), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_SKIP), FALSE);
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    const auto [ok, msg] = VerifyApiSettings(model, api_url, api_key);

    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_PURCHASE), TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_VERIFY), TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_CONFIG_SKIP), TRUE);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (!api_key.empty()) {
        ctx->config_ui.empty_key_verified = false;
        if (allow_status_update) {
            SetConfigStatus(hwnd, ok ? (msg.empty() ? s.at(L"verify_success") : msg)
                                     : FormatOneArg(s.at(L"verify_fail"), msg.empty() ? L"Unknown error" : msg));
        }
        return ok;
    }

    if (ok) {
        ctx->config_ui.empty_key_verified = true;
        if (allow_status_update) {
            SetConfigStatus(hwnd, s.at(L"verify_empty_success"));
        }
        return true;
    }
    ctx->config_ui.empty_key_verified = false;
    if (allow_status_update) {
        SetConfigStatus(hwnd, FormatOneArg(s.at(L"verify_fail"), msg.empty() ? L"Unknown error" : msg));
    }
    return false;
}

} // namespace
