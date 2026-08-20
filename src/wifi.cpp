#include "wifi.h"

#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <windows.h>

namespace {

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring hex(const std::vector<BYTE>& bytes) {
    std::wstringstream out;
    for (BYTE value : bytes) {
        out << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(value);
    }
    return out.str();
}

std::wstring trim_xml(std::wstring value) {
    if (!value.empty() && value.front() == 0xFEFF) value.erase(value.begin());
    std::wstring result;
    result.reserve(value.size());
    bool previous_cr = false;
    for (wchar_t ch : value) {
        if (ch == L'\r') {
            result.push_back(L'\n');
            previous_cr = true;
        } else if (ch == L'\n') {
            if (!previous_cr) result.push_back(ch);
            previous_cr = false;
        } else {
            result.push_back(ch);
            previous_cr = false;
        }
    }
    while (!result.empty() && iswspace(result.front())) result.erase(result.begin());
    while (!result.empty() && iswspace(result.back())) result.pop_back();
    return result;
}

bool write_atomic(const std::filesystem::path& path, const std::string& data) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto temp = path.wstring() + L".tmp";
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
        && written == data.size()
        && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

}

bool WifiSyncManager::initialize(const std::filesystem::path& storage, int interval_seconds, bool log_sensitive_names, bool sync_on_enable, bool sync_on_service_start) {
    storage_ = storage / L"wifi";
    profiles_dir_ = storage_ / L"profiles";
    pending_dir_ = storage_ / L"pending";
    interval_seconds_ = std::max(1, interval_seconds);
    log_sensitive_names_ = log_sensitive_names;
    sync_on_enable_ = sync_on_enable;
    sync_on_service_start_ = sync_on_service_start;

    DWORD negotiated = 0;
    const DWORD result = WlanOpenHandle(2, nullptr, &negotiated, &wlan_handle_);
    if (result != ERROR_SUCCESS) return false;
    wlan_version_ = negotiated;
    std::error_code error;
    std::filesystem::create_directories(profiles_dir_, error);
    std::filesystem::create_directories(pending_dir_, error);
    if (error || !load_global()) {
        WlanCloseHandle(wlan_handle_, nullptr);
        wlan_handle_ = nullptr;
        return false;
    }
    save_metadata();
    const DWORD notification_result = WlanRegisterNotification(
        wlan_handle_,
        WLAN_NOTIFICATION_SOURCE_ACM,
        TRUE,
        &WifiSyncManager::notification_callback,
        this,
        nullptr,
        nullptr);
    initialized_ = notification_result == ERROR_SUCCESS;
    dirty_ = sync_on_service_start_;
    propagate_pending_ = sync_on_service_start_;
    next_scan_tick_ = sync_on_service_start_ ? 0 : GetTickCount64() + static_cast<uint64_t>(interval_seconds_) * 1000;
    return initialized_;
}

void WifiSyncManager::shutdown() {
    if (wlan_handle_ != nullptr) {
        WlanRegisterNotification(wlan_handle_, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, nullptr);
        WlanCloseHandle(wlan_handle_, nullptr);
    }
    wlan_handle_ = nullptr;
    initialized_ = false;
}

void WifiSyncManager::mark_dirty() {
    dirty_ = true;
    propagate_pending_ = true;
}

bool WifiSyncManager::available() const {
    return initialized_ && wlan_handle_ != nullptr;
}

size_t WifiSyncManager::global_profile_count() const {
    return global_profiles_.size();
}

void WifiSyncManager::tick(bool force_scan) {
    if (!available()) return;
    const uint64_t now = GetTickCount64();
    if (!force_scan && !dirty_.load() && now < next_scan_tick_) return;
    std::vector<WifiProfileRecord> local;
    if (enumerate_local(local)) {
        discover_offline_profiles(local);
        merge_global(local);
        if (propagate_pending_.load() || force_scan) {
            if (propagate()) propagate_pending_ = false;
        }
    }
    dirty_ = false;
    next_scan_tick_ = now + static_cast<uint64_t>(interval_seconds_) * 1000;
}

bool WifiSyncManager::discover_offline_profiles(std::vector<WifiProfileRecord>& profiles) {
    const std::filesystem::path root = L"C:\\ProgramData\\Microsoft\\Wlansvc\\Profiles\\Interfaces";
    std::error_code error;
    if (!std::filesystem::exists(root, error)) return !error;
    for (const auto& interface_dir : std::filesystem::directory_iterator(root, error)) {
        if (error || !interface_dir.is_directory()) continue;
        const std::wstring interface_id = interface_dir.path().filename().wstring();
        if (known_interface_guids_.find(interface_id) != known_interface_guids_.end()) continue;
        for (const auto& file : std::filesystem::directory_iterator(interface_dir.path(), error)) {
            if (error || !file.is_regular_file() || file.path().extension() != L".xml") continue;
            std::ifstream input(file.path(), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(input)), {});
            if (bytes.empty()) continue;
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (size <= 0) continue;
            WifiProfileRecord record;
            record.name = file.path().stem().wstring();
            record.xml.resize(size);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), record.xml.data(), size);
            record.hash = sha256_text(trim_xml(record.xml));
            record.observed_tick = GetTickCount64();
            profiles.push_back(std::move(record));
        }
    }
    return true;
}

bool WifiSyncManager::enumerate_local(std::vector<WifiProfileRecord>& profiles) {
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    const DWORD result = WlanEnumInterfaces(wlan_handle_, nullptr, &interfaces);
    if (result != ERROR_SUCCESS || interfaces == nullptr) return false;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        const auto& info = interfaces->InterfaceInfo[i];
        const bool connected = info.isState == wlan_interface_state_connected;
        const std::wstring interface_id = guid_text(info.InterfaceGuid);
        known_interface_guids_.insert(interface_id);
        read_profile_list(info.InterfaceGuid, info.isState, connected, profiles);
    }
    WlanFreeMemory(interfaces);
    return true;
}

bool WifiSyncManager::read_profile_list(const GUID& interface_guid, WLAN_INTERFACE_STATE state, bool connected, std::vector<WifiProfileRecord>& profiles) {
    PWLAN_PROFILE_INFO_LIST list = nullptr;
    if (WlanGetProfileList(wlan_handle_, &interface_guid, nullptr, &list) != ERROR_SUCCESS || list == nullptr) return false;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const auto& info = list->ProfileInfo[i];
        LPWSTR xml = nullptr;
        DWORD flags = 0;
        DWORD granted = 0;
        const DWORD result = WlanGetProfile(wlan_handle_, &interface_guid, info.strProfileName, nullptr, &xml, &flags, &granted);
        if (result == ERROR_SUCCESS && xml != nullptr) {
            WifiProfileRecord record;
            record.name = info.strProfileName;
            record.xml = xml;
            record.hash = sha256_text(trim_xml(record.xml));
            record.interface_guid = interface_guid;
            record.interface_state = state;
            record.connected = connected;
            record.user_profile = (flags & WLAN_PROFILE_USER) != 0;
            record.group_policy = (flags & WLAN_PROFILE_GROUP_POLICY) != 0;
            record.granted_access = granted;
            record.observed_tick = GetTickCount64();
            profiles.push_back(std::move(record));
            WlanFreeMemory(xml);
        }
    }
    WlanFreeMemory(list);
    return true;
}

bool WifiSyncManager::merge_global(const std::vector<WifiProfileRecord>& local_profiles) {
    bool changed = false;
    for (const auto& local : local_profiles) {
        auto it = global_profiles_.find(local.hash);
        bool local_changed = false;
        const auto old_hash = it == global_profiles_.end() ? std::wstring{} : it->second.hash;
        if (it == global_profiles_.end()) {
            WifiGlobalProfile global;
            global.identity = stable_identity(local.name, local.xml);
            global.name = local.name;
            global.xml = local.xml;
            global.hash = local.hash;
            global.last_observed_tick = local.observed_tick;
            global.connected_observed = local.connected;
            global.user_profile = local.user_profile;
            global.group_policy = local.group_policy;
            global.propagatable = !local.group_policy;
            global.source_interfaces.insert(guid_text(local.interface_guid));
            global_profiles_.emplace(global.hash, std::move(global));
            changed = true;
        } else {
            local_changed = it->second.hash != local.hash;
            it->second.xml = local.xml;
            it->second.hash = local.hash;
            it->second.connected_observed = local.connected;
            it->second.user_profile = local.user_profile;
            it->second.group_policy = local.group_policy;
            it->second.propagatable = !local.group_policy;
            it->second.last_observed_tick = std::max(it->second.last_observed_tick, local.observed_tick);
            it->second.source_interfaces.insert(guid_text(local.interface_guid));
            changed = changed || local_changed;
        }
    }
    if (!changed) return true;
    for (const auto& [hash, profile] : global_profiles_) {
        if (!save_global_profile(profile)) return false;
    }
    return save_metadata();
}

bool WifiSyncManager::load_global() {
    global_profiles_.clear();
    std::error_code error;
    if (!std::filesystem::exists(profiles_dir_, error)) return !error;
    for (const auto& entry : std::filesystem::directory_iterator(profiles_dir_, error)) {
        if (error || !entry.is_regular_file() || entry.path().extension() != L".xml") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.empty()) continue;
        int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (size <= 0) continue;
        std::wstring xml(size, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), xml.data(), size);
        WifiGlobalProfile profile;
        profile.xml = xml;
        profile.hash = sha256_text(trim_xml(xml));
        profile.name = extract_tag(xml, L"name");
        if (profile.name.empty()) profile.name = entry.path().stem().wstring();
        profile.identity = stable_identity(profile.name, xml);
        global_profiles_[profile.hash] = std::move(profile);
    }
    return true;
}

bool WifiSyncManager::save_global_profile(const WifiGlobalProfile& profile) {
    const auto path = profiles_dir_ / profile_filename(profile.hash);
    return write_atomic(path, utf8(profile.xml));
}

bool WifiSyncManager::save_metadata() {
    std::wstring content = L"[WiFi]\r\nProfileCount=" + std::to_wstring(global_profiles_.size()) + L"\r\n";
    for (const auto& [identity, profile] : global_profiles_) {
        content += L"ProfileHash=" + profile.hash + L"\r\n";
    }
    return write_atomic(storage_ / L"metadata.ini", utf8(content));
}

bool WifiSyncManager::propagate() {
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(wlan_handle_, nullptr, &interfaces) != ERROR_SUCCESS || interfaces == nullptr) return false;

    std::map<std::wstring, const WifiGlobalProfile*> masters;
    for (const auto& [_, profile] : global_profiles_) {
        if (!profile.propagatable) continue;
        auto it = masters.find(profile.name);
        if (it == masters.end()) {
            masters.emplace(profile.name, &profile);
            continue;
        }
        const WifiGlobalProfile* current = it->second;
        if ((!current->connected_observed && profile.connected_observed)
            || (current->connected_observed == profile.connected_observed
                && profile.last_observed_tick > current->last_observed_tick)) {
            it->second = &profile;
        }
    }

    bool ok = true;
    for (const auto& [_, profile] : masters) {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const auto& info = interfaces->InterfaceInfo[i];
            if (!set_profile_on_interface(info.InterfaceGuid, *profile)) ok = false;
        }
    }
    WlanFreeMemory(interfaces);
    return ok;
}

bool WifiSyncManager::set_profile_on_interface(const GUID& interface_guid, const WifiGlobalProfile& profile) {
    DWORD reason = 0;
    const DWORD result = WlanSetProfile(
        wlan_handle_,
        &interface_guid,
        0,
        profile.xml.c_str(),
        nullptr,
        TRUE,
        nullptr,
        &reason);
    if (result == ERROR_SUCCESS) return true;
    const auto pending = pending_dir_ / (profile_filename(profile.hash) + L"." + guid_text(interface_guid) + L".pending");
    write_atomic(pending, utf8(profile.hash));
    return false;
}

void WifiSyncManager::notification(const WLAN_NOTIFICATION_DATA* data) {
    if (data == nullptr || data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM) return;
    const bool enable_event = data->NotificationCode == wlan_notification_acm_autoconf_enabled
        || data->NotificationCode == wlan_notification_acm_interface_arrival
        || data->NotificationCode == wlan_notification_acm_connection_complete
        || data->NotificationCode == wlan_notification_acm_profile_change;
    if (enable_event && sync_on_enable_) {
        mark_dirty();
    }
}

void WINAPI WifiSyncManager::notification_callback(PWLAN_NOTIFICATION_DATA data, PVOID context) {
    if (context != nullptr) static_cast<WifiSyncManager*>(context)->notification(data);
}

std::wstring WifiSyncManager::extract_tag(const std::wstring& xml, const std::wstring& tag) {
    const std::wstring open = L"<" + tag + L">";
    const std::wstring close = L"</" + tag + L">";
    const size_t begin = xml.find(open);
    if (begin == std::wstring::npos) return {};
    const size_t value_begin = begin + open.size();
    const size_t end = xml.find(close, value_begin);
    if (end == std::wstring::npos) return {};
    return xml.substr(value_begin, end - value_begin);
}

std::wstring WifiSyncManager::stable_identity(const std::wstring& name, const std::wstring& xml) {
    const std::wstring ssid = extract_tag(xml, L"name");
    const std::wstring authentication = extract_tag(xml, L"authentication");
    const std::wstring encryption = extract_tag(xml, L"encryption");
    const std::wstring connection_mode = extract_tag(xml, L"connectionMode");
    return name + L"|" + ssid + L"|" + authentication + L"|" + encryption + L"|" + connection_mode;
}

std::wstring WifiSyncManager::sha256_text(const std::wstring& text) {
    const std::string bytes = utf8(text);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0
        || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &returned, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<BYTE> object(object_size);
    std::vector<BYTE> digest(32);
    const bool ok = BCryptCreateHash(algorithm, &hash, object.data(), object.size(), nullptr, 0, 0) == 0
        && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) == 0
        && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? hex(digest) : L"";
}

std::wstring WifiSyncManager::guid_text(const GUID& guid) {
    wchar_t buffer[64]{};
    return StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) > 0 ? buffer : L"unknown";
}

std::wstring WifiSyncManager::profile_filename(const std::wstring& hash) {
    return hash + L".xml";
}

bool WifiSyncManager::same_guid(const GUID& a, const GUID& b) {
    return IsEqualGUID(a, b) != FALSE;
}
