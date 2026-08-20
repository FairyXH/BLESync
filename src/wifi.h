#pragma once

#include <windows.h>
#include <wlanapi.h>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <atomic>

struct WifiProfileRecord {
    std::wstring name;
    std::wstring xml;
    std::wstring hash;
    GUID interface_guid{};
    WLAN_INTERFACE_STATE interface_state = wlan_interface_state_not_ready;
    bool connected = false;
    bool user_profile = false;
    bool group_policy = false;
    DWORD granted_access = 0;
    uint64_t observed_tick = 0;
};

struct WifiGlobalProfile {
    std::wstring identity;
    std::wstring name;
    std::wstring xml;
    std::wstring hash;
    uint64_t last_observed_tick = 0;
    bool connected_observed = false;
    bool user_profile = false;
    bool group_policy = false;
    bool propagatable = true;
    std::set<std::wstring> source_interfaces;
};

class WifiSyncManager {
public:
    bool initialize(const std::filesystem::path& storage, int interval_seconds, bool log_sensitive_names, bool sync_on_enable, bool sync_on_service_start);
    void shutdown();
    void mark_dirty();
    void tick(bool force_scan = false);
    bool available() const;
    size_t global_profile_count() const;

private:
    bool enumerate_local(std::vector<WifiProfileRecord>& profiles);
    bool discover_offline_profiles(std::vector<WifiProfileRecord>& profiles);
    bool read_profile_list(const GUID& interface_guid, WLAN_INTERFACE_STATE state, bool connected, std::vector<WifiProfileRecord>& profiles);
    bool merge_global(const std::vector<WifiProfileRecord>& local_profiles);
    bool load_global();
    bool save_global_profile(const WifiGlobalProfile& profile);
    bool save_metadata();
    bool propagate();
    bool set_profile_on_interface(const GUID& interface_guid, const WifiGlobalProfile& profile);
    void notification(const WLAN_NOTIFICATION_DATA* data);
    static void WINAPI notification_callback(PWLAN_NOTIFICATION_DATA data, PVOID context);
    static std::wstring stable_identity(const std::wstring& name, const std::wstring& xml);
    static std::wstring extract_tag(const std::wstring& xml, const std::wstring& tag);
    static std::wstring sha256_text(const std::wstring& text);
    static std::wstring guid_text(const GUID& guid);
    static std::wstring profile_filename(const std::wstring& hash);
    static bool same_guid(const GUID& a, const GUID& b);

    HANDLE wlan_handle_ = nullptr;
    DWORD wlan_version_ = 0;
    std::filesystem::path storage_;
    std::filesystem::path profiles_dir_;
    std::filesystem::path pending_dir_;
    std::map<std::wstring, WifiGlobalProfile> global_profiles_;
    std::set<std::wstring> known_interface_guids_;
    std::atomic<bool> dirty_{true};
    std::atomic<bool> propagate_pending_{true};
    bool initialized_ = false;
    bool log_sensitive_names_ = false;
    bool sync_on_enable_ = true;
    bool sync_on_service_start_ = true;
    int interval_seconds_ = 5;
    uint64_t next_scan_tick_ = 0;
};
