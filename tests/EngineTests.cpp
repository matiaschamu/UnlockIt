#include "../UnlockEngine.h"
#include <aclapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <fcntl.h>
#include <io.h>

namespace fs = std::filesystem;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const fs::path& path) { std::ofstream file(path); file << "Contenido de prueba\n"; Check(file.good(), "create fixture"); }
struct Fixture {
    fs::path root = fs::current_path() / L"x64" / (L"test-data-" + std::to_wstring(GetCurrentProcessId()));
    Fixture() { fs::create_directories(root / L"interior" / L"más adentro"); }
    ~Fixture() {
        // Only remove the uniquely named fixture created inside the workspace.
        std::error_code error;
        for (auto it = fs::recursive_directory_iterator(root, error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
            if (it->is_regular_file()) SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
        fs::remove_all(root, error);
    }
};
struct DenyPermission {
    fs::path path;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL original = nullptr, replacement = nullptr;
    DenyPermission(const fs::path& target, DWORD rights) : path(target) {
        Check(GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &original, nullptr, &descriptor) == ERROR_SUCCESS, "read original delete permissions");
        BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD size = sizeof(sid);
        Check(CreateWellKnownSid(WinWorldSid, nullptr, sid, &size), "create delete permission SID");
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = rights;
        entry.grfAccessMode = DENY_ACCESS;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        Check(SetEntriesInAclW(1, &entry, original, &replacement) == ERROR_SUCCESS, "build delete restriction");
        Check(SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, replacement, nullptr) == ERROR_SUCCESS, "apply delete restriction");
    }
    ~DenyPermission() {
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, original, nullptr);
        LocalFree(replacement); LocalFree(descriptor);
    }
};
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--analyze") {
        const auto text = unlock::Describe(unlock::Analyze({ argv[2] })) + L"\r\n";
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
        _setmode(_fileno(stdout), _O_BINARY);
        std::cout.write(utf8.data(), utf8.size());
        return 0;
    }
    if (argc == 5 && (std::wstring(argv[1]) == L"--hold-lock" || std::wstring(argv[1]) == L"--hold-directory" || std::wstring(argv[1]) == L"--hold-cwd")) {
        const bool directory = std::wstring(argv[1]) == L"--hold-directory";
        const bool cwd = std::wstring(argv[1]) == L"--hold-cwd";
        HANDLE file = cwd ? INVALID_HANDLE_VALUE : CreateFileW(argv[2], directory ? FILE_LIST_DIRECTORY : GENERIC_READ | GENERIC_WRITE,
            directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : 0, nullptr, OPEN_EXISTING,
            directory ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
        if (cwd ? !SetCurrentDirectoryW(argv[2]) : file == INVALID_HANDLE_VALUE) return 2;
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
        HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
        SetEvent(ready);
        WaitForSingleObject(release, 30000);
        CloseHandle(ready); CloseHandle(release);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return 0;
    }
    try {
        Fixture fixture;
        const auto ordinary = fixture.root / L"normal.txt";
        const auto blocked = fixture.root / L"interior" / L"más adentro" / L"archivo con espacios.txt";
        Write(ordinary); Write(blocked);
        { std::ofstream stream(blocked.wstring() + L":Zone.Identifier"); stream << "[ZoneTransfer]\r\nZoneId=3\r\n"; Check(stream.good(), "create internet mark"); }
        Check(SetFileAttributesW(blocked.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN), "mark read-only");
        const auto firstFolder = fixture.root / L"interior";
        const auto deepFolder = firstFolder / L"más adentro";
        Check(SetFileAttributesW(firstFolder.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN), "mark inner folder read-only");
        Check(SetFileAttributesW(deepFolder.c_str(), FILE_ATTRIBUTE_READONLY), "mark nested folder read-only");
        auto report = unlock::Analyze({ fixture.root.wstring(), blocked.wstring() });
        Check(report.files.size() == 2 && report.folders == 3, "recursive traversal and duplicate selection");
        Check(report.internet == 1 && report.readOnly == 1, "detect nested restrictions");
        Check(report.readOnlyFolders.size() == 2 && report.deleteDenied == 0, "folder read-only is distinct from deletion permissions");
        Check(unlock::Describe(report).find(L"archivo con espacios.txt") != std::wstring::npos, "diagnostic includes internal path");
        Check(GetFileAttributesW(blocked.c_str()) & FILE_ATTRIBUTE_READONLY, "analysis is read-only");
        std::wstring outcome;
        report = unlock::Unblock({ fixture.root.wstring() }, {}, outcome);
        if (report.internet || report.readOnly || !report.readOnlyFolders.empty() || report.errors) std::wcerr << outcome << unlock::Describe(report) << L'\n';
        Check(report.internet == 0 && report.readOnly == 0 && report.readOnlyFolders.empty() && report.errors == 0, "recursive unlock and rescan");
        Check(GetFileAttributesW(blocked.c_str()) & FILE_ATTRIBUTE_HIDDEN, "preserve unrelated attributes");
        Check(GetFileAttributesW(firstFolder.c_str()) & FILE_ATTRIBUTE_HIDDEN, "preserve other folder attributes");
        std::ifstream original(blocked); std::string line; std::getline(original, line); original.close();
        Check(line == "Contenido de prueba", "preserve file contents");
        std::cout << "PASS: recursion, Unicode paths, deduplication, diagnostic, unblock, content preservation\n";

        report = unlock::Analyze({ (fixture.root / L"missing.txt").wstring() });
        Check(report.errors == 1 && !report.actionable(), "missing path diagnostic");
        std::stop_source stopped; stopped.request_stop();
        Check(unlock::Analyze({ fixture.root.wstring() }, stopped.get_token()).cancelled, "cancellation");
        std::cout << "PASS: missing paths and cancellation\n";

        // Deny only attribute writes on a synthetic file, restoring the original DACL immediately.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL oldAcl = nullptr, deniedAcl = nullptr;
        Check(GetNamedSecurityInfoW(const_cast<LPWSTR>(ordinary.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &oldAcl, nullptr, &descriptor) == ERROR_SUCCESS, "read fixture permissions");
        BYTE sidBuffer[SECURITY_MAX_SID_SIZE]; DWORD sidSize = sizeof(sidBuffer);
        Check(CreateWellKnownSid(WinWorldSid, nullptr, sidBuffer, &sidSize), "create fixture SID");
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = FILE_WRITE_ATTRIBUTES;
        entry.grfAccessMode = DENY_ACCESS;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sidBuffer);
        Check(SetEntriesInAclW(1, &entry, oldAcl, &deniedAcl) == ERROR_SUCCESS, "build fixture permissions");
        Check(SetNamedSecurityInfoW(const_cast<LPWSTR>(ordinary.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, deniedAcl, nullptr) == ERROR_SUCCESS, "apply fixture permissions");
        report = unlock::Analyze({ ordinary.wstring() });
        DWORD restored = SetNamedSecurityInfoW(const_cast<LPWSTR>(ordinary.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, oldAcl, nullptr);
        LocalFree(deniedAcl); LocalFree(descriptor);
        Check(restored == ERROR_SUCCESS && report.needsAdmin && report.errors > 0, "permission diagnosis and admin request");
        std::cout << "PASS: access denial requests administrator\n";

        {
            DenyPermission child(firstFolder, DELETE);
            report = unlock::Analyze({ fixture.root.wstring() });
            Check(report.deleteDenied == 0, "parent delete-child right permits removal despite child denial");
            {
                DenyPermission parent(fixture.root, FILE_DELETE_CHILD);
                report = unlock::Analyze({ fixture.root.wstring() });
                Check(report.deleteDenied >= 1 && report.needsAdmin, "detect internal folder without deletion rights");
                Check(unlock::Describe(report).find(firstFolder.wstring()) != std::wstring::npos, "permission diagnosis shows exact internal path");
                Check(fs::exists(blocked), "deletion checks do not remove content");
            }
        }
        Check(unlock::Analyze({ fixture.root.wstring() }).deleteDenied == 0, "restored folder deletion rights");
        std::cout << "PASS: nested folder deletion permissions, parent fallback and non-destructive analysis\n";

        std::wstring readyName = L"Local\\UnlockItTestReady" + std::to_wstring(GetCurrentProcessId());
        std::wstring releaseName = L"Local\\UnlockItTestRelease" + std::to_wstring(GetCurrentProcessId());
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --hold-lock \"" + blocked.wstring() + L"\" " + readyName + L" " + releaseName;
        STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION child{};
        Check(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child), "start synthetic lock holder");
        const DWORD readyResult = WaitForSingleObject(ready, 10000);
        report = unlock::Analyze({ fixture.root.wstring() });
        SetEvent(release);
        WaitForSingleObject(child.hProcess, 10000);
        CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(ready); CloseHandle(release);
        Check(readyResult == WAIT_OBJECT_0, "lock fixture ready");
        Check(report.inUse >= 1 && !report.processes.empty(), "identify nested lock and holder");
        Check(report.processes.front().identity.dwProcessId == child.dwProcessId, "correct locking PID");
        Check(unlock::Analyze({ fixture.root.wstring() }).inUse == 0, "detect released lock");
        std::cout << "PASS: nested exclusive lock, exact holder PID and released lock\n";

        ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        const auto inner = fixture.root / L"interior";
        command = L"\"" + std::wstring(exe) + L"\" --hold-directory \"" + inner.wstring() + L"\" " + readyName + L" " + releaseName;
        Check(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child), "start directory lock holder");
        const DWORD directoryReady = WaitForSingleObject(ready, 10000);
        report = unlock::Analyze({ fixture.root.wstring() });
        SetEvent(release);
        WaitForSingleObject(child.hProcess, 10000);
        CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(ready); CloseHandle(release);
        Check(directoryReady == WAIT_OBJECT_0 && report.sharingBlocks > 0, "detect locked internal directory");
        Check(report.directoriesInUse == 1 && report.directoryQueryErrors == 0, "identify directory-holding processes");
        bool directoryHolderFound = false;
        for (const auto& process : report.processes)
            if (process.identity.dwProcessId == child.dwProcessId && process.directoryReference) directoryHolderFound = true;
        Check(directoryHolderFound && report.canClosePrograms() && report.actionable(), "directory candidates can be individually authorized");
        Check(unlock::Describe(report).find(L"interior") != std::wstring::npos, "locked directory path");
        Check(unlock::Analyze({ fixture.root.wstring() }).directoriesInUse == 0, "released directory lock disappears");
        std::cout << "PASS: locked internal directory and exact holder\n";

        ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        command = L"\"" + std::wstring(exe) + L"\" --hold-cwd \"" + fixture.root.wstring() + L"\" " + readyName + L" " + releaseName;
        Check(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child), "start current-directory holder");
        const DWORD cwdReady = WaitForSingleObject(ready, 10000);
        const auto secondReadyName = readyName + L"Second";
        const auto secondReleaseName = releaseName + L"Second";
        HANDLE secondReady = CreateEventW(nullptr, TRUE, FALSE, secondReadyName.c_str());
        HANDLE secondRelease = CreateEventW(nullptr, TRUE, FALSE, secondReleaseName.c_str());
        command = L"\"" + std::wstring(exe) + L"\" --hold-cwd \"" + fixture.root.wstring() + L"\" " + secondReadyName + L" " + secondReleaseName;
        PROCESS_INFORMATION secondChild{};
        Check(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &secondChild), "start second directory holder");
        const DWORD secondReadyResult = WaitForSingleObject(secondReady, 10000);
        report = unlock::Analyze({ fixture.root.wstring() });
        bool cwdHolderFound = false;
        bool secondHolderFound = false;
        unlock::Process cwdHolder{}, secondHolder{};
        for (const auto& process : report.processes)
            if (process.identity.dwProcessId == child.dwProcessId && process.directoryReference) {
                cwdHolderFound = true;
                cwdHolder = process;
            } else if (process.identity.dwProcessId == secondChild.dwProcessId && process.directoryReference) {
                secondHolderFound = true;
                secondHolder = process;
            }
        std::wstring closeOutcome;
        const auto noConsent = unlock::Unblock({ fixture.root.wstring() }, {}, closeOutcome);
        const bool unapprovedAlive = WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT;
        auto staleHolder = cwdHolder;
        staleHolder.identity.ProcessStartTime.dwLowDateTime ^= 1;
        unlock::Unblock({ fixture.root.wstring() }, { staleHolder }, closeOutcome);
        const bool staleAlive = WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT;
        unlock::Report afterClose;
        if (cwdHolderFound) afterClose = unlock::Unblock({ fixture.root.wstring() }, { cwdHolder }, closeOutcome);
        const bool approvedExited = WaitForSingleObject(child.hProcess, 1000) == WAIT_OBJECT_0;
        const bool secondUnapprovedAlive = WaitForSingleObject(secondChild.hProcess, 0) == WAIT_TIMEOUT;
        unlock::Report afterSecondClose;
        if (secondHolderFound) afterSecondClose = unlock::Unblock({ fixture.root.wstring() }, { secondHolder }, closeOutcome);
        const bool secondApprovedExited = WaitForSingleObject(secondChild.hProcess, 1000) == WAIT_OBJECT_0;
        SetEvent(release); WaitForSingleObject(child.hProcess, 10000);
        SetEvent(secondRelease); WaitForSingleObject(secondChild.hProcess, 10000);
        CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(ready); CloseHandle(release);
        CloseHandle(secondChild.hThread); CloseHandle(secondChild.hProcess); CloseHandle(secondReady); CloseHandle(secondRelease);
        Check(cwdReady == WAIT_OBJECT_0 && secondReadyResult == WAIT_OBJECT_0 && report.sharingBlocks > 0 && cwdHolderFound && secondHolderFound, "identify every current-directory holder");
        Check(unapprovedAlive && noConsent.sharingBlocks > 0, "unapproved process stays open");
        Check(staleAlive, "stale process identity is never terminated");
        Check(approvedExited && secondUnapprovedAlive && afterClose.sharingBlocks > 0, "only the individually approved process is terminated");
        Check(secondApprovedExited && afterSecondClose.sharingBlocks == 0 && fs::exists(blocked), "second approval releases directory without deleting data");
        Check(unlock::Analyze({ fixture.root.wstring() }).sharingBlocks == 0, "released current-directory lock disappears");
        std::cout << "PASS: current-directory lock, per-process consent, stale identity and authorized termination\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
