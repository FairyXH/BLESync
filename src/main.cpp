#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <bcrypt.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <iostream>
#include <functional>
#include "wifi.h"

static const GUID BLE_DEVICE_CLASS_GUID = {
    0xe0cbf06c,
    0xcd8b,
    0x4647,
    {0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74}
};

#include "security.cpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static constexpr wchar_t SERVICE_NAME[] = L"BLESync";
static constexpr wchar_t DEV_ROOT[] = L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices";
static constexpr wchar_t KEY_ROOT[] = L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Keys";
static std::atomic<bool> g_stop{false};
static SERVICE_STATUS_HANDLE g_status_handle = nullptr;
static SERVICE_STATUS g_status{};
static WifiSyncManager g_wifi;

struct Config {
    fs::path exe_dir, storage;
    int interval = 5;
    std::wstring log_level = L"INFO";
    bool wifi_enabled = true;
    bool wifi_sync_on_start = true;
    bool wifi_sync_on_enable = true;
    int wifi_interval = 5;
    bool log_sensitive_names = false;
    bool bluetooth_enabled = true;
    bool bluetooth_sync_on_start = true;
    bool bluetooth_sync_on_enable = true;
};
struct Value { DWORD type = REG_NONE; std::vector<BYTE> data; };
struct Key { std::wstring name; std::map<std::wstring, Value> values; std::map<std::wstring, Key> children; };
struct Snapshot {
    Key root;
    std::wstring hash;
    uint64_t bytes = 0;
};

struct SyncMetadata {
    std::wstring machine_id;
    uint64_t version = 0;
    uint64_t updated_tick = 0;
    std::wstring origin;
};

static std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), towlower);
    return s;
}

static std::wstring upper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), towupper);
    return s;
}

static fs::path module_dir() {
    wchar_t buffer[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    return fs::path(std::wstring(buffer, length)).parent_path();
}

static Config load_config() {
    Config c; c.exe_dir = module_dir(); const auto ini = (c.exe_dir / L"BLESync.ini").wstring(); wchar_t buffer[32768]{};
    GetPrivateProfileStringW(L"BLESync", L"StoragePath", L"", buffer, 32768, ini.c_str()); c.storage = fs::path(buffer);
    GetPrivateProfileStringW(L"BLESync", L"LogLevel", L"INFO", buffer, 32768, ini.c_str()); c.log_level = upper(buffer);
    GetPrivateProfileStringW(L"BLESync", L"ScanInterval", L"5", buffer, 32768, ini.c_str()); try { c.interval = std::max(1, std::stoi(buffer)); } catch (...) { c.interval = 5; }
    GetPrivateProfileStringW(L"WiFi", L"Enabled", L"true", buffer, 32768, ini.c_str()); c.wifi_enabled = lower(buffer) != L"false" && lower(buffer) != L"0";
    GetPrivateProfileStringW(L"WiFi", L"ScanInterval", L"5", buffer, 32768, ini.c_str()); try { c.wifi_interval = std::max(1, std::stoi(buffer)); } catch (...) { c.wifi_interval = 5; }
    GetPrivateProfileStringW(L"WiFi", L"SyncOnServiceStart", L"true", buffer, 32768, ini.c_str()); c.wifi_sync_on_start = lower(buffer) != L"false" && lower(buffer) != L"0";
    GetPrivateProfileStringW(L"WiFi", L"SyncOnWiFiEnable", L"true", buffer, 32768, ini.c_str()); c.wifi_sync_on_enable = lower(buffer) != L"false" && lower(buffer) != L"0";
    GetPrivateProfileStringW(L"BLESync", L"LogSensitiveNames", L"false", buffer, 32768, ini.c_str()); c.log_sensitive_names = lower(buffer) == L"true" || lower(buffer) == L"1";
    GetPrivateProfileStringW(L"Bluetooth", L"Enabled", L"true", buffer, 32768, ini.c_str()); c.bluetooth_enabled = lower(buffer) != L"false" && lower(buffer) != L"0";
    GetPrivateProfileStringW(L"Bluetooth", L"SyncOnServiceStart", L"true", buffer, 32768, ini.c_str()); c.bluetooth_sync_on_start = lower(buffer) != L"false" && lower(buffer) != L"0";
    GetPrivateProfileStringW(L"Bluetooth", L"SyncOnBluetoothEnable", L"true", buffer, 32768, ini.c_str()); c.bluetooth_sync_on_enable = lower(buffer) != L"false" && lower(buffer) != L"0";
    if (c.storage.empty()) { c.storage = c.exe_dir / L"BLESyncData"; }
    return c;
}

static std::string wide_to_utf8(const std::wstring& value) { if (value.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr); std::string result(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr); return result; }

static std::wstring format_message(const wchar_t* level, const std::wstring& message) { SYSTEMTIME t{}; GetLocalTime(&t); std::wstringstream out; out << L"[" << std::setfill(L'0') << std::setw(2) << t.wHour << L":" << std::setw(2) << t.wMinute << L":" << std::setw(2) << t.wSecond << L"] [" << level << L"] " << message; return out.str(); }

class Logger {
    std::mutex mutex_; fs::path path_;
public:
    void init(const Config& c) { path_ = c.storage / L"logs" / L"BLESync.log"; std::error_code e; fs::create_directories(path_.parent_path(), e); }
    void write(const wchar_t* level, const std::wstring& message) {
        std::lock_guard<std::mutex> lock(mutex_); const auto line = wide_to_utf8(format_message(level, message)) + "\r\n"; HANDLE h = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (h == INVALID_HANDLE_VALUE) return; DWORD written = 0; WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr); CloseHandle(h);
    }
};
static Logger g_log;

static bool is_admin() {
    BOOL result = FALSE; PSID sid = nullptr; SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) { CheckTokenMembership(nullptr, sid, &result); FreeSid(sid); }
    return result != FALSE;
}

static bool protect_storage(const fs::path& path) {
    return ProtectStorage(path);
}

static std::wstring new_instance_id() {
    GUID guid{};
    if (CoCreateGuid(&guid) != S_OK) {
        return L"unknown-" + std::to_wstring(GetCurrentProcessId());
    }
    wchar_t text[64]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    return text;
}

static std::wstring read_or_create_instance_id(const fs::path& storage) {
    const auto path = (storage / L"state" / L"instance.ini").wstring();
    wchar_t value[128]{};
    GetPrivateProfileStringW(L"Instance", L"MachineId", L"", value, 128, path.c_str());
    if (value[0] != L'\0') {
        return value;
    }
    const std::wstring id = new_instance_id();
    fs::create_directories(storage / L"state");
    WritePrivateProfileStringW(L"Instance", L"MachineId", id.c_str(), path.c_str());
    return id;
}

static void u32(std::ostream& out, uint32_t value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
static bool ru32(std::istream& in, uint32_t& value) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value))); }
static void text(std::ostream& out, const std::wstring& value) { u32(out, static_cast<uint32_t>(value.size())); out.write(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t)); }
static bool rtext(std::istream& in, std::wstring& value) { uint32_t n = 0; if (!ru32(in, n) || n > 1000000) return false; value.resize(n); return static_cast<bool>(in.read(reinterpret_cast<char*>(value.data()), n * sizeof(wchar_t))); }
static void encode_key(std::ostream& out, const Key& key) { text(out, key.name); u32(out, static_cast<uint32_t>(key.values.size())); for (const auto& [name, val] : key.values) { text(out, name); u32(out, val.type); u32(out, static_cast<uint32_t>(val.data.size())); if (!val.data.empty()) out.write(reinterpret_cast<const char*>(val.data.data()), val.data.size()); } u32(out, static_cast<uint32_t>(key.children.size())); for (const auto& [_, child] : key.children) encode_key(out, child); }
static bool decode_key(std::istream& in, Key& key) { uint32_t n = 0; if (!rtext(in, key.name) || !ru32(in, n) || n > 100000) return false; for (uint32_t i = 0; i < n; ++i) { std::wstring name; Value value; uint32_t type = 0, size = 0; if (!rtext(in, name) || !ru32(in, type) || !ru32(in, size) || size > 128 * 1024 * 1024) return false; value.type = type; value.data.resize(size); if (size && !in.read(reinterpret_cast<char*>(value.data.data()), size)) return false; key.values.emplace(std::move(name), std::move(value)); } if (!ru32(in, n) || n > 100000) return false; for (uint32_t i = 0; i < n; ++i) { Key child; if (!decode_key(in, child)) return false; key.children.emplace(child.name, std::move(child)); } return true; }

static std::wstring sha256(const std::vector<BYTE>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; DWORD object_size = 0, result = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 || BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result, 0) != 0) { if (alg) BCryptCloseAlgorithmProvider(alg, 0); return L""; }
    std::vector<BYTE> object(object_size), digest(32); if (BCryptCreateHash(alg, &hash, object.data(), object.size(), nullptr, 0, 0) != 0 || BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) != 0 || BCryptFinishHash(hash, digest.data(), digest.size(), 0) != 0) { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); return L""; }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0); std::wstringstream out; for (BYTE b : digest) out << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(b); return out.str();
}

static bool capture_registry(const wchar_t* root, Snapshot& snap) {
    HKEY handle = nullptr; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &handle) != ERROR_SUCCESS) return false;
    snap.root = {}; snap.root.name = root;
    std::function<bool(HKEY, Key&)> read = [&](HKEY h, Key& key) {
        DWORD subkeys = 0, values = 0; if (RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) return false;
        for (DWORD i = 0; i < values; ++i) { wchar_t name[1024]{}; DWORD name_len = 1024, type = 0, size = 0; if (RegEnumValueW(h, i, name, &name_len, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) continue; Value v; v.type = type; v.data.resize(size); name_len = 1024; if (RegEnumValueW(h, i, name, &name_len, nullptr, &v.type, v.data.data(), &size) == ERROR_SUCCESS) { v.data.resize(size); key.values.emplace(name, std::move(v)); } }
        for (DWORD i = 0; i < subkeys; ++i) { wchar_t name[1024]{}; DWORD len = 1024; if (RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) continue; HKEY child_handle = nullptr; if (RegOpenKeyExW(h, name, 0, KEY_READ | KEY_WOW64_64KEY, &child_handle) != ERROR_SUCCESS) continue; Key child; child.name = name; const bool ok = read(child_handle, child); RegCloseKey(child_handle); if (!ok) return false; key.children.emplace(child.name, std::move(child)); }
        return true;
    };
    const bool ok = read(handle, snap.root); RegCloseKey(handle); return ok;
}

static bool serialize_snapshot(const Snapshot& snap, std::vector<BYTE>& payload) { std::ostringstream out(std::ios::out | std::ios::binary); out.write("BLESNAP2", 8); u32(out, 2); encode_key(out, snap.root); const auto value = out.str(); payload.assign(value.begin(), value.end()); return true; }
static bool save_metadata(const fs::path& storage, const std::wstring& devices_hash, const std::wstring& keys_hash, const std::wstring& machine_id, uint64_t version) {
    const auto state = storage / L"state";
    std::error_code error;
    fs::create_directories(state, error);
    if (error) {
        return false;
    }
    const auto path = state / L"metadata.ini";
    const auto temp = state / L"metadata.ini.tmp";
    const std::wstring content = L"[Meta]\r\n"
        L"MachineId=" + machine_id + L"\r\n"
        L"SnapshotVersion=" + std::to_wstring(version) + L"\r\n"
        L"DevicesHash=" + devices_hash + L"\r\n"
        L"KeysHash=" + keys_hash + L"\r\n"
        L"UpdatedTick=" + std::to_wstring(GetTickCount64()) + L"\r\n";
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::string utf8 = wide_to_utf8(content);
    DWORD written = 0;
    const bool ok = WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr)
        && written == utf8.size()
        && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

static uint64_t read_snapshot_version(const fs::path& storage) {
    wchar_t value[64]{};
    GetPrivateProfileStringW(L"Meta", L"SnapshotVersion", L"0", value, 64, (storage / L"state" / L"metadata.ini").c_str());
    try {
        return std::stoull(value);
    } catch (...) {
        return 0;
    }
}

static bool save_snapshot(const fs::path& path, const Snapshot& snap, std::wstring* saved_hash = nullptr) {
    std::vector<BYTE> payload;
    if (!serialize_snapshot(snap, payload)) {
        return false;
    }
    const auto hash = sha256(payload);
    if (saved_hash != nullptr) {
        *saved_hash = hash;
    }
    const fs::path temp = path.wstring() + L".tmp";
    const fs::path backup = path.wstring() + L".backup";
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)
        && written == payload.size()
        && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!wrote) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES
        && !MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

static bool save_pair(
    const fs::path& storage,
    const Snapshot& devices,
    const Snapshot& keys,
    const std::wstring& machine_id,
    uint64_t version) {
    std::wstring devices_hash;
    std::wstring keys_hash;
    if (!save_snapshot(storage / L"registry" / L"devices.regdata", devices, &devices_hash)
        || !save_snapshot(storage / L"registry" / L"keys.regdata", keys, &keys_hash)) {
        return false;
    }
    return save_metadata(storage, devices_hash, keys_hash, machine_id, version);
}

static bool load_snapshot(const fs::path& path, Snapshot& snap) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<BYTE> payload((std::istreambuf_iterator<char>(in)), {});
    if (payload.size() < 12 || std::memcmp(payload.data(), "BLESNAP2", 8) != 0) {
        return false;
    }
    uint32_t version = 0;
    std::istringstream data(std::string(payload.begin(), payload.end()), std::ios::in | std::ios::binary);
    char magic[8]{};
    data.read(magic, 8);
    if (!ru32(data, version) || version != 2 || !decode_key(data, snap.root)) {
        return false;
    }
    char trailing = 0;
    if (data.read(&trailing, 1)) {
        return false;
    }
    snap.hash = sha256(payload);
    snap.bytes = payload.size();
    if (snap.hash.empty()) {
        return false;
    }
    wchar_t expected[256]{};
    const auto ini = (path.parent_path().parent_path() / L"state" / L"metadata.ini").wstring();
    const bool is_keys = lower(path.filename().wstring()).find(L"keys") != std::wstring::npos;
    const wchar_t* hash_key = is_keys ? L"KeysHash" : L"DevicesHash";
    GetPrivateProfileStringW(L"Meta", hash_key, L"", expected, 256, ini.c_str());
    return expected[0] != L'\0' && snap.hash == expected;
}
static bool equal_key(const Key& a, const Key& b) {
    if (a.values.size() != b.values.size() || a.children.size() != b.children.size()) {
        return false;
    }
    for (const auto& [name, value] : a.values) {
        const auto it = b.values.find(name);
        if (it == b.values.end()
            || value.type != it->second.type
            || value.data != it->second.data) {
            return false;
        }
    }
    for (const auto& [name, child] : a.children) {
        const auto it = b.children.find(name);
        if (it == b.children.end() || !equal_key(child, it->second)) {
            return false;
        }
    }
    return true;
}

static std::wstring redact_identifier(const std::wstring& value) {
    if (value.size() <= 4) {
        return L"****";
    }
    if (value.size() <= 8) {
        return value.substr(0, 2) + L"****" + value.substr(value.size() - 2);
    }
    return value.substr(0, 4) + L"****" + value.substr(value.size() - 4);
}

static std::wstring registry_type_name(DWORD type) {
    switch (type) {
    case REG_SZ: return L"REG_SZ";
    case REG_EXPAND_SZ: return L"REG_EXPAND_SZ";
    case REG_BINARY: return L"REG_BINARY";
    case REG_DWORD: return L"REG_DWORD";
    case REG_QWORD: return L"REG_QWORD";
    case REG_MULTI_SZ: return L"REG_MULTI_SZ";
    default: return L"REG_" + std::to_wstring(type);
    }
}

static void log_device_list(const Key& root, const wchar_t* title) {
    std::wstring text = title;
    text += L"（数量=" + std::to_wstring(root.children.size()) + L"）：";
    if (root.children.empty()) {
        text += L"无";
    } else {
        bool first = true;
        for (const auto& [name, child] : root.children) {
            if (!first) {
                text += L"、";
            }
            text += redact_identifier(name);
            first = false;
        }
    }
    g_log.write(L"信息", text);
}

static void log_snapshot_diff(const Key& before, const Key& after, const wchar_t* scope) {
    std::wstring prefix = std::wstring(L"注册表变化[" ) + scope + L"] ";
    for (const auto& [name, child] : after.children) {
        if (before.children.find(name) == before.children.end()) {
            g_log.write(L"信息", prefix + L"增加：" + redact_identifier(name));
        }
    }
    for (const auto& [name, child] : before.children) {
        if (after.children.find(name) == after.children.end()) {
            g_log.write(L"信息", prefix + L"删除：" + redact_identifier(name));
        }
    }
    for (const auto& [name, child] : after.children) {
        const auto old = before.children.find(name);
        if (old == before.children.end()) {
            continue;
        }
        for (const auto& [value_name, value] : child.values) {
            const auto old_value = old->second.values.find(value_name);
            if (old_value == old->second.values.end()) {
                g_log.write(L"信息", prefix + L"设备 " + redact_identifier(name)
                    + L" 增加值：" + value_name + L"，类型=" + registry_type_name(value.type)
                    + L"，长度=" + std::to_wstring(value.data.size()));
            } else if (old_value->second.type != value.type || old_value->second.data != value.data) {
                g_log.write(L"信息", prefix + L"设备 " + redact_identifier(name)
                    + L" 修改值：" + value_name + L"，类型=" + registry_type_name(value.type)
                    + L"，长度=" + std::to_wstring(value.data.size()));
            }
        }
        for (const auto& [value_name, value] : old->second.values) {
            if (child.values.find(value_name) == child.values.end()) {
                g_log.write(L"信息", prefix + L"设备 " + redact_identifier(name)
                    + L" 删除值：" + value_name);
            }
        }
    }
}

struct RegistryWriteSummary {
    size_t set_values = 0;
    size_t deleted_values = 0;
    size_t visited_keys = 0;
    size_t deleted_keys = 0;
};
static bool apply_key(
    HKEY parent,
    const Key& desired,
    bool allow_delete,
    RegistryWriteSummary& summary
) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(
            parent,
            desired.name.c_str(),
            0,
            nullptr,
            0,
            KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
            nullptr,
            &h,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }
    ++summary.visited_keys;

    DWORD value_count = 0;
    RegQueryInfoKeyW(
        h,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &value_count,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    std::set<std::wstring> current_values;
    for (DWORD i = 0; i < value_count; ++i) {
        wchar_t name[1024]{};
        DWORD length = 1024;
        if (RegEnumValueW(h, i, name, &length, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            current_values.emplace(name);
        }
    }

    for (const auto& [name, value] : desired.values) {
        if (RegSetValueExW(
                h,
                name.c_str(),
                0,
                value.type,
                value.data.data(),
                static_cast<DWORD>(value.data.size())) != ERROR_SUCCESS) {
            RegCloseKey(h);
            return false;
        }
        ++summary.set_values;
        current_values.erase(name);
    }
    if (allow_delete) {
        for (const auto& name : current_values) {
            if (RegDeleteValueW(h, name.c_str()) == ERROR_SUCCESS) {
                ++summary.deleted_values;
            }
        }
    }

    DWORD subkeys = 0;
    RegQueryInfoKeyW(
        h,
        nullptr,
        nullptr,
        nullptr,
        &subkeys,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    std::set<std::wstring> current_children;
    for (DWORD i = 0; i < subkeys; ++i) {
        wchar_t name[1024]{};
        DWORD length = 1024;
        if (RegEnumKeyExW(h, i, name, &length, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            current_children.emplace(name);
        }
    }
    for (const auto& [name, child] : desired.children) {
        if (!apply_key(h, child, allow_delete, summary)) {
            RegCloseKey(h);
            return false;
        }
        current_children.erase(name);
    }
    if (allow_delete) {
        for (const auto& name : current_children) {
            if (RegDeleteTreeW(h, name.c_str()) == ERROR_SUCCESS) {
                ++summary.deleted_keys;
            }
        }
    }
    RegCloseKey(h);
    return true;
}

static bool restore_registry(
    const wchar_t* root,
    const Snapshot& snap,
    RegistryWriteSummary& summary
) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            root,
            0,
            nullptr,
            0,
            KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
            nullptr,
            &h,
            nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = true;
    for (const auto& [name, child] : snap.root.children) {
        ok = ok && apply_key(h, child, true, summary);
    }
    RegCloseKey(h);
    return ok;
}

static SERVICE_STATUS_PROCESS service_state(const wchar_t* name) { SERVICE_STATUS_PROCESS result{}; SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!manager) return result; SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS); if (service) { DWORD bytes = 0; QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&result), sizeof(result), &bytes); CloseServiceHandle(service); } CloseServiceHandle(manager); return result; }
static bool radio_state(bool& known, bool& enabled) { known = false; enabled = false; HDEVINFO info = SetupDiGetClassDevsW(&BLE_DEVICE_CLASS_GUID, nullptr, nullptr, DIGCF_PRESENT); if (info == INVALID_HANDLE_VALUE) return false; SP_DEVINFO_DATA data{}; data.cbSize = sizeof(data); for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &data); ++i) { DWORD status = 0, problem = 0; if (CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) == CR_SUCCESS) { known = true; enabled = (status & DN_STARTED) != 0 && problem == 0; break; } } SetupDiDestroyDeviceInfoList(info); return known; }
static void report_service(DWORD state, DWORD error = NO_ERROR, DWORD hint = 0) { g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS; g_status.dwCurrentState = state; g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0; g_status.dwWin32ExitCode = error; g_status.dwWaitHint = hint; SetServiceStatus(g_status_handle, &g_status); }

static bool install_service() {
    if (!is_admin()) {
        return false;
    }
    const Config c = load_config();
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!manager) {
        return false;
    }
    const auto exe = (c.exe_dir / L"BLESync.exe").wstring();
    SC_HANDLE service = OpenServiceW(manager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!service) {
        service = CreateServiceW(
            manager,
            SERVICE_NAME,
            L"BLESync Bluetooth Persistence Service",
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            exe.c_str(),
            nullptr,
            nullptr,
            nullptr,
            L"LocalSystem",
            nullptr);
    }
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }
    if (!ChangeServiceConfigW(
            service,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            exe.c_str(),
            nullptr,
            nullptr,
            nullptr,
            L"LocalSystem",
            nullptr,
            L"BLESync Bluetooth Persistence Service")) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
    SC_ACTION actions[3]{{SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 120000}, {SC_ACTION_RESTART, 300000}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    const bool started = StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return started;
}
static bool uninstall_service() { if (!is_admin()) return false; SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS); if (!manager) return false; SC_HANDLE service = OpenServiceW(manager, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE); if (!service) { CloseServiceHandle(manager); return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST; } SERVICE_STATUS status{}; ControlService(service, SERVICE_CONTROL_STOP, &status); const bool ok = DeleteService(service) != FALSE; CloseServiceHandle(service); CloseServiceHandle(manager); return ok; }

static void worker() {
    const Config config = load_config();
    g_log.init(config);

    std::error_code error;
    fs::create_directories(config.storage / L"registry", error);
    if (error || !protect_storage(config.storage)) {
        g_log.write(L"错误", L"Storage 初始化或 ACL 设置失败，服务工作线程已停止");
        return;
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\BLESync.StorageLock");
    if (!mutex) {
        g_log.write(L"错误", L"无法创建 Storage 互斥体");
        return;
    }

    const DWORD lock_result = WaitForSingleObject(mutex, 2000);
    if (lock_result != WAIT_OBJECT_0 && lock_result != WAIT_ABANDONED) {
        g_log.write(L"错误", L"等待 Storage 互斥体超时，已安全停止工作线程");
        CloseHandle(mutex);
        return;
    }

    if (config.wifi_enabled) {
        if (g_wifi.initialize(config.storage, config.wifi_interval, config.log_sensitive_names)) {
            g_log.write(L"信息", L"Wi-Fi 同步模块已初始化，Global Profiles=" + std::to_wstring(g_wifi.global_profile_count()));
            if (config.wifi_sync_on_start) g_wifi.tick(true);
        } else {
            g_log.write(L"警告", L"Wi-Fi WLAN API 初始化失败，Bluetooth 继续独立运行");
        }
    }

    protect_storage(config.storage);

    const std::wstring machine_id = read_or_create_instance_id(config.storage);
    uint64_t snapshot_version = read_snapshot_version(config.storage);
    bool restoring = false;
    std::wstring expected_devices_hash;
    std::wstring expected_keys_hash;
    uint64_t restore_deadline = 0;

    g_log.write(L"信息", L"服务已启动，InstanceId=" + machine_id);

    bool have_local = false;
    Snapshot local_devices;
    Snapshot local_keys;
    const fs::path devices_file = config.storage / L"registry" / L"devices.regdata";
    const fs::path keys_file = config.storage / L"registry" / L"keys.regdata";

    while (!g_stop) {
        if (config.bluetooth_enabled) {
            const auto service = service_state(L"bthserv");
        bool radio_known = false;
        bool radio_enabled = false;
        radio_state(radio_known, radio_enabled);

        if (service.dwCurrentState == SERVICE_START_PENDING
            || service.dwCurrentState == SERVICE_STOP_PENDING) {
            g_log.write(L"调试", L"Bluetooth 服务正在切换状态，暂缓同步");
        } else if (service.dwCurrentState == SERVICE_RUNNING
            && radio_known
            && radio_enabled) {
            Snapshot now_devices;
            Snapshot now_keys;
            const bool got_devices = capture_registry(DEV_ROOT, now_devices);
            const bool got_keys = capture_registry(KEY_ROOT, now_keys);

            if (got_devices && got_keys) {
                Snapshot stored_devices;
                Snapshot stored_keys;
                const bool has_stored = load_snapshot(devices_file, stored_devices)
                    && load_snapshot(keys_file, stored_keys);

                bool restored = false;
                if (!has_stored || !have_local) {
                    if (has_stored) {
                        const bool local_differs = !equal_key(now_devices.root, stored_devices.root)
                            || !equal_key(now_keys.root, stored_keys.root);
                        if (!local_differs) {
                            local_devices = std::move(now_devices);
                            local_keys = std::move(now_keys);
                            have_local = true;
                        } else if (!have_local) {
                            RegistryWriteSummary devices_write;
                            RegistryWriteSummary keys_write;
                            restoring = true;
                            restore_deadline = GetTickCount64() + 30000;
                            expected_devices_hash = stored_devices.hash;
                            expected_keys_hash = stored_keys.hash;
                            if (restore_registry(DEV_ROOT, stored_devices, devices_write)
                                && restore_registry(KEY_ROOT, stored_keys, keys_write)) {
                                g_log.write(
                                    L"信息",
                                    L"已向系统写入蓝牙注册表：Devices 设置值="
                                        + std::to_wstring(devices_write.set_values)
                                        + L"，删除值=" + std::to_wstring(devices_write.deleted_values)
                                        + L"，访问键=" + std::to_wstring(devices_write.visited_keys)
                                        + L"，删除键=" + std::to_wstring(devices_write.deleted_keys)
                                        + L"；Keys 设置值="
                                        + std::to_wstring(keys_write.set_values)
                                        + L"，删除值=" + std::to_wstring(keys_write.deleted_values)
                                        + L"，访问键=" + std::to_wstring(keys_write.visited_keys)
                                        + L"，删除键=" + std::to_wstring(keys_write.deleted_keys));
                                restored = true;
                            }
                        }
                    }

                    if (restored) {
                        Snapshot restored_devices;
                        Snapshot restored_keys;
                        if (capture_registry(DEV_ROOT, restored_devices)
                            && capture_registry(KEY_ROOT, restored_keys)) {
                            local_devices = std::move(restored_devices);
                            local_keys = std::move(restored_keys);
                            have_local = true;
                            g_log.write(L"信息", L"有效的持久化蓝牙状态已恢复并通过验证");
                            log_device_list(local_devices.root, L"当前已配对设备");
                        }
                    } else {
                        ++snapshot_version;
                        save_pair(config.storage, now_devices, now_keys, machine_id, snapshot_version);
                        local_devices = std::move(now_devices);
                        local_keys = std::move(now_keys);
                        have_local = true;
                        g_log.write(L"信息", L"本地蓝牙状态已发布为基线");
                        log_device_list(local_devices.root, L"当前已配对设备");
                    }
                } else if (!equal_key(local_devices.root, now_devices.root)
                    || !equal_key(local_keys.root, now_keys.root)) {
                    log_snapshot_diff(local_devices.root, now_devices.root, L"Devices");
                    log_snapshot_diff(local_keys.root, now_keys.root, L"Keys");
                    ++snapshot_version;
                    save_pair(config.storage, now_devices, now_keys, machine_id, snapshot_version);
                    local_devices = std::move(now_devices);
                    local_keys = std::move(now_keys);
                    g_log.write(L"信息", L"稳定的本地蓝牙变化已发布，新增和删除均已镜像");
                    log_device_list(local_devices.root, L"当前已配对设备");
                } else if (restoring && GetTickCount64() < restore_deadline) {
                    g_log.write(L"调试", L"等待恢复后的注册表状态稳定，抑制回写循环");
                } else if (restoring) {
                    restoring = false;
                    g_log.write(L"警告", L"恢复验证窗口超时，停止自动回写并保留当前状态");
                } else if (!equal_key(now_devices.root, stored_devices.root)
                    || !equal_key(now_keys.root, stored_keys.root)) {
                    g_log.write(
                        L"警告",
                        L"持久化状态与本地状态不一致，按用户优先策略保留本地状态并记录冲突");
                    log_device_list(now_devices.root, L"当前本地已配对设备");
                    log_device_list(stored_devices.root, L"持久化设备记录");
                }
            } else {
                g_log.write(L"调试", L"蓝牙快照不可用，不执行恢复或服务控制");
            }
        } else if (!radio_known || !radio_enabled) {
            g_log.write(L"调试", L"蓝牙适配器不可用或已禁用，不自动启用或重启服务");
        }

        }

        if (config.wifi_enabled && g_wifi.available()) {
            g_wifi.tick(false);
        }

        for (int i = 0; i < config.interval && !g_stop; ++i) {
            if (!g_stop) {
                std::this_thread::sleep_for(1s);
            }
        }
    }

    g_wifi.shutdown();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    g_log.write(L"信息", L"服务工作线程已停止");
}
static void WINAPI service_control(DWORD code) { if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) { g_stop = true; report_service(SERVICE_STOP_PENDING, NO_ERROR, 5000); } }
static void WINAPI service_main(DWORD, LPWSTR*) { g_status_handle = RegisterServiceCtrlHandlerW(SERVICE_NAME, service_control); if (!g_status_handle) return; report_service(SERVICE_START_PENDING, NO_ERROR, 5000); std::thread thread(worker); report_service(SERVICE_RUNNING); thread.join(); report_service(SERVICE_STOPPED); }

static bool request_elevation(const wchar_t* argument) {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (length == 0 || length >= std::size(module)) {
        return false;
    }
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = module;
    execute.lpParameters = argument;
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        return false;
    }
    if (execute.hProcess != nullptr) {
        WaitForSingleObject(execute.hProcess, INFINITE);
        DWORD exit_code = 1;
        GetExitCodeProcess(execute.hProcess, &exit_code);
        CloseHandle(execute.hProcess);
        return exit_code == 0;
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        const std::wstring arg = lower(argv[1]);
        if (arg == L"--help" || arg == L"-h" || arg == L"/?" || arg == L"/h") {
            std::wcout
                << L"BLESync - Windows 蓝牙配置持久化服务\n\n"
                << L"用法：\n"
                << L"  BLESync.exe --help     显示此帮助\n"
                << L"  BLESync.exe /?         显示此帮助\n"
                << L"  BLESync.exe --install  安装或更新并启动 BLESync 服务\n"
                << L"  BLESync.exe --uninstall 卸载 BLESync 服务\n"
                << L"  BLESync.exe --status   显示 Bluetooth 服务和适配器状态\n"
                << L"  BLESync.exe --capture 以当前权限捕获 Devices/Keys 快照\n"
                << L"  BLESync.exe --console 以前台模式运行服务工作线程\n\n"
                << L"配置文件：BLESync.ini（必须与 EXE 位于同一目录）\n"
                << L"服务名：BLESync\n"
                << L"服务账户：LocalSystem\n";
            return 0;
        }
        if (arg == L"--install") {
            if (!is_admin()) {
                return request_elevation(L"--install") ? 0 : 1;
            }
            return install_service() ? 0 : 1;
        }
        if (arg == L"--uninstall") {
            if (!is_admin()) {
                return request_elevation(L"--uninstall") ? 0 : 1;
            }
            return uninstall_service() ? 0 : 1;
        }
        if (arg == L"--console") {
            worker();
            return 0;
        }
        if (arg == L"--capture") {
            const Config c = load_config();
            Snapshot devices;
            Snapshot keys;
            return capture_registry(DEV_ROOT, devices)
                    && capture_registry(KEY_ROOT, keys)
                    && save_pair(c.storage, devices, keys, read_or_create_instance_id(c.storage), read_snapshot_version(c.storage) + 1)
                ? 0
                : 1;
        }
        if (arg == L"--status") {
            const auto service = service_state(L"bthserv");
            bool known = false;
            bool enabled = false;
            radio_state(known, enabled);
            std::wcout
                << L"Bluetooth 服务状态：" << service.dwCurrentState << L"\n"
                << L"Bluetooth 适配器已知：" << (known ? L"是" : L"否") << L"\n"
                << L"Bluetooth 适配器已启用：" << (enabled ? L"是" : L"否") << L"\n";
            return 0;
        }
        std::wcerr << L"错误：未知参数。使用 --help 或 /? 查看用法。\n";
        return 2;
    }
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(SERVICE_NAME), service_main},
        {nullptr, nullptr}
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            if (!is_admin()) {
                return request_elevation(L"--install") ? 0 : 1;
            }
            return install_service() ? 0 : 1;
        }
        return 1;
    }
    return 0;
}