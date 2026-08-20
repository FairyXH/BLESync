#include <windows.h>
#include <sddl.h>
#include <filesystem>
namespace fs = std::filesystem;

static bool ProtectStorage(const fs::path& root) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) return false;
    if (!fs::exists(root)) fs::create_directories(root);
    const bool ok = SetFileSecurityW(root.c_str(), DACL_SECURITY_INFORMATION, sd) != FALSE;
    LocalFree(sd);
    return ok;
}
