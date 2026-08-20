#include <windows.h>
#include <sddl.h>
#include <filesystem>
namespace fs = std::filesystem;

static bool ProtectStorage(const fs::path& root) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
            SDDL_REVISION_1,
            &sd,
            nullptr)) {
        return false;
    }
    std::error_code error;
    fs::create_directories(root, error);
    if (error || SetFileSecurityW(root.c_str(), DACL_SECURITY_INFORMATION, sd) == FALSE) {
        LocalFree(sd);
        return false;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root, error)) {
        if (error) {
            LocalFree(sd);
            return false;
        }
        if (SetFileSecurityW(entry.path().c_str(), DACL_SECURITY_INFORMATION, sd) == FALSE) {
            LocalFree(sd);
            return false;
        }
    }
    LocalFree(sd);
    return true;
}
