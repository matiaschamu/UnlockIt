#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <restartmanager.h>
#include <string>
#include <vector>
#include <stop_token>

namespace unlock {
struct File {
    std::wstring path;
    bool readOnly = false;
    bool internet = false;
    bool metadataOnly = false;
};
struct Process {
    RM_UNIQUE_PROCESS identity{};
    std::wstring name;
    bool directoryReference = false;
};
struct Report {
    std::vector<File> files;
    std::vector<Process> processes;
    std::vector<std::wstring> readOnlyFolders;
    std::vector<std::wstring> details;
    size_t folders = 0, readOnly = 0, internet = 0, errors = 0, skipped = 0, inUse = 0, sharingBlocks = 0;
    size_t deleteDenied = 0, cloudItems = 0, cloudErrors = 0, metadataOnly = 0;
    size_t directoriesInUse = 0, directoryQueryErrors = 0;
    bool needsAdmin = false, cancelled = false;
    bool canClosePrograms() const { return !processes.empty(); }
    bool actionable() const { return readOnly || internet || !readOnlyFolders.empty() || canClosePrograms(); }
};
Report Analyze(const std::vector<std::wstring>& roots, std::stop_token stop = {});
Report Unblock(const std::vector<std::wstring>& roots, const std::vector<Process>& approved,
    std::wstring& outcome, std::stop_token stop = {});
std::wstring Describe(const Report& report);
std::wstring ErrorText(DWORD error);
bool IsAdministrator();
}
