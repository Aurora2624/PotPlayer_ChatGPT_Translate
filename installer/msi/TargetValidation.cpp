// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only MSI custom actions. No files, registry values, credentials, network
// requests or processes are created. MSI's standard actions install the payload.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <cwchar>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kUpgradeCode[] = L"{6C47A458-771E-5B7D-9CDF-880CDEC12830}";
constexpr wchar_t kTranslateSuffix[] = L"\\Extension\\Subtitle\\Translate";
struct PayloadFile { const wchar_t* name; const wchar_t* component; };
constexpr PayloadFile kFiles[] = {
    {L"SubtitleTranslate - ChatGPT.as", L"{1C5EF9FB-C2F8-5B87-A5D8-5F387A11613F}"},
    {L"SubtitleTranslate - ChatGPT.ico", L"{9627DCEB-AD03-5D1F-A18B-47BAAE5BC047}"},
    {L"SubtitleTranslate - ChatGPT - Without Context.as", L"{DFFD87D6-13CE-56E8-A327-C04F1DB8951A}"},
    {L"SubtitleTranslate - ChatGPT - Without Context.ico", L"{69380DB8-612E-549F-A27D-C9C289DDE688}"},
};

std::wstring Property(MSIHANDLE session, const wchar_t* name) {
    DWORD size = 0;
    wchar_t empty = L'\0';
    const UINT status = MsiGetPropertyW(session, name, &empty, &size);
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) return {};
    std::vector<wchar_t> value(static_cast<size_t>(size) + 1);
    ++size;
    if (MsiGetPropertyW(session, name, value.data(), &size) != ERROR_SUCCESS) return {};
    return std::wstring(value.data(), size);
}

std::wstring Normalize(const std::wstring& input) {
    // Only fully qualified local drive paths. Exclude UNC/device paths and ADS.
    if (input.size() < 3 || input[1] != L':' || input[2] != L'\\' ||
        !((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z')) ||
        input.find(L':', 2) != std::wstring::npos) return {};
    const DWORD size = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (size == 0 || size > 32767) return {};
    std::vector<wchar_t> buffer(size);
    const DWORD copied = GetFullPathNameW(input.c_str(), size, buffer.data(), nullptr);
    if (copied == 0 || copied >= size) return {};
    std::wstring path(buffer.data(), copied);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool EqualPath(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && !right.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool RegularFile(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool IsPotPlayerTarget(const std::wstring& path) {
    const size_t suffixLength = std::wcslen(kTranslateSuffix);
    if (path.size() <= suffixLength ||
        _wcsicmp(path.c_str() + path.size() - suffixLength, kTranslateSuffix) != 0) return false;
    const std::wstring root = path.substr(0, path.size() - suffixLength);
    if (!RegularFile(root + L"\\PotPlayerMini64.exe") && !RegularFile(root + L"\\PotPlayerMini.exe")) return false;
    // Existing ancestors must be real directories, not symlinks or junctions.
    // The expected plugin subdirectories may be missing in a real installation.
    for (size_t end = 3; end <= path.size();) {
        const DWORD attr = GetFileAttributesW(path.substr(0, end).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT))) return false;
        const size_t next = path.find(L'\\', end + 1);
        if (end == path.size()) break;
        end = next == std::wstring::npos ? path.size() : next;
    }
    return true;
}

std::vector<std::wstring> RelatedProducts() {
    std::vector<std::wstring> products;
    wchar_t product[39] = {};
    for (DWORD i = 0; MsiEnumRelatedProductsW(kUpgradeCode, 0, i, product) == ERROR_SUCCESS; ++i) {
        products.emplace_back(product);
    }
    return products;
}

std::wstring RegisteredFile(const std::wstring& product, const wchar_t* component) {
    DWORD size = 0;
    wchar_t empty = L'\0';
    const INSTALLSTATE initial = MsiGetComponentPathW(product.c_str(), component, &empty, &size);
    if (size == 0 || (initial != INSTALLSTATE_MOREDATA && initial != INSTALLSTATE_LOCAL &&
                      initial != INSTALLSTATE_ABSENT)) return {};
    std::vector<wchar_t> path(static_cast<size_t>(size) + 1);
    ++size;
    const INSTALLSTATE state = MsiGetComponentPathW(product.c_str(), component, path.data(), &size);
    if (state != INSTALLSTATE_LOCAL && state != INSTALLSTATE_ABSENT) return {};
    return Normalize(std::wstring(path.data(), size));
}

std::wstring InstalledFolder(const std::vector<std::wstring>& products) {
    for (const auto& product : products) {
        for (const auto& file : kFiles) {
            const auto path = RegisteredFile(product, file.component);
            const auto slash = path.find_last_of(L'\\');
            if (slash != std::wstring::npos) return path.substr(0, slash);
        }
    }
    return {};
}

UINT Fail(MSIHANDLE session, const wchar_t* message) {
    PMSIHANDLE record = MsiCreateRecord(0);
    MsiRecordSetStringW(record, 0, message);
    MsiProcessMessage(session, INSTALLMESSAGE_ERROR, record);
    return ERROR_INSTALL_FAILURE;
}
} // namespace

extern "C" __declspec(dllexport) UINT __stdcall DetectPotPlayer(MSIHANDLE session) {
    try {
        // Preserve an explicit command-line/UI target. It is always revalidated.
        if (!Property(session, L"INSTALLFOLDER").empty()) return ERROR_SUCCESS;
        const auto installed = InstalledFolder(RelatedProducts());
        if (!installed.empty()) return MsiSetPropertyW(session, L"INSTALLFOLDER", installed.c_str());
        for (const auto& programFiles : {L"ProgramFiles64Folder", L"ProgramFilesFolder"}) {
            const auto base = Normalize(Property(session, programFiles));
            if (base.empty()) continue;
            for (const auto& relative : {L"\\DAUM\\PotPlayer", L"\\PotPlayer"}) {
                const auto candidate = base + relative + kTranslateSuffix;
                if (IsPotPlayerTarget(candidate)) return MsiSetPropertyW(session, L"INSTALLFOLDER", candidate.c_str());
            }
        }
        // UI still offers the conventional ProgramFiles64Folder target. The
        // execute-sequence check rejects it unless PotPlayer is actually there.
        return ERROR_SUCCESS;
    } catch (...) {
        return Fail(session, L"Unable to inspect the PotPlayer installation folder.");
    }
}

extern "C" __declspec(dllexport) UINT __stdcall ValidatePotPlayer(MSIHANDLE session) {
    try {
        const auto folder = Normalize(Property(session, L"INSTALLFOLDER"));
        if (!IsPotPlayerTarget(folder)) {
            return Fail(session, L"Select the Extension\\Subtitle\\Translate folder below an existing PotPlayer installation (containing PotPlayerMini64.exe or PotPlayerMini.exe). Network paths, junctions and symbolic links are not supported. No plugin files were changed.");
        }
        const auto products = RelatedProducts();
        const auto installed = InstalledFolder(products);
        if (!installed.empty() && !EqualPath(installed, folder)) {
            return Fail(session, L"This MSI is already installed to another PotPlayer folder. Upgrade or repair in that original folder, or uninstall it before changing the destination.");
        }
        for (const auto& file : kFiles) {
            const auto target = folder + L"\\" + file.name;
            if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            bool owned = false;
            for (const auto& product : products) {
                if (EqualPath(RegisteredFile(product, file.component), target)) owned = true;
            }
            if (!owned || !RegularFile(target)) {
                return Fail(session, L"A target plugin file already exists and is not owned by this MSI, or is a directory/link. Back up custom changes and uninstall the other installer (or move manually installed plugin files) before switching to MSI. No plugin files were changed.");
            }
        }
        return ERROR_SUCCESS;
    } catch (...) {
        return Fail(session, L"Unable to validate the PotPlayer installation folder. No plugin files were changed.");
    }
}
