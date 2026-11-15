#include "UnlockEngine.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <winternl.h>
#include <cstring>

#pragma comment(lib, "Rstrtmgr.lib")
#pragma comment(lib, "Advapi32.lib")

namespace unlock {
namespace {
struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
struct Session {
    DWORD id = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    DWORD error = RmStartSession(&id, 0, key);
    ~Session() { if (error == ERROR_SUCCESS) RmEndSession(id); }
};
struct PathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
void Detail(Report& r, const std::wstring& text) {
    if (r.details.size() < 200) r.details.push_back(text);
}
void Failure(Report& r, const std::wstring& path, DWORD error) {
    ++r.errors;
    r.needsAdmin |= error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
    Detail(r, path + L"\r\n  " + ErrorText(error));
    switch (error) {
    case ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING:
    case ERROR_CLOUD_FILE_PROVIDER_TERMINATED:
    case ERROR_CLOUD_FILE_NOT_IN_SYNC:
    case ERROR_CLOUD_FILE_READ_ONLY_VOLUME:
    case ERROR_CLOUD_FILE_AUTHENTICATION_FAILED:
    case ERROR_CLOUD_FILE_NETWORK_UNAVAILABLE:
    case ERROR_CLOUD_FILE_IN_USE:
    case ERROR_CLOUD_FILE_ACCESS_DENIED:
    case ERROR_CLOUD_FILE_PROPERTY_LOCK_CONFLICT:
    case ERROR_CLOUD_FILE_REQUEST_ABORTED:
    case ERROR_CLOUD_FILE_REQUEST_CANCELED:
    case ERROR_CLOUD_FILE_REQUEST_TIMEOUT:
    case ERROR_CLOUD_FILE_UNSUCCESSFUL:
        ++r.cloudErrors;
        Detail(r, path + L"\r\n  El proveedor de nube informó un problema que puede impedir borrar o modificar este elemento. Abrí OneDrive o el proveedor correspondiente y revisá su estado de sincronización, conexión y permisos.");
        break;
    }
}
bool Missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_INVALID_NAME || error == ERROR_NOT_SUPPORTED;
}
std::wstring FullPath(const std::wstring& path) {
    DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size) return path;
    std::wstring result(size, L'\0');
    DWORD copied = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
    if (!copied || copied >= size) return path;
    result.resize(copied);
    if (result.starts_with(L"\\\\?\\")) return result;
    if (result.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + result.substr(2);
    return L"\\\\?\\" + result;
}
std::wstring DisplayPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}
bool CloudReparse(const std::wstring& path, DWORD& error) {
    Handle handle{ CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
    if (handle.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle.value, FileAttributeTagInfo, &info, sizeof(info))) {
        error = GetLastError(); return false;
    }
    error = ERROR_SUCCESS;
    return (info.ReparseTag & ~IO_REPARSE_TAG_CLOUD_MASK) == IO_REPARSE_TAG_CLOUD;
}
bool CheckDeletion(Report& r, const std::wstring& path, DWORD attributes) {
    // Request the right only; never mark an item for deletion.
    Handle item{ CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
    if (item.value != INVALID_HANDLE_VALUE) return false;
    DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        ++r.sharingBlocks;
        Detail(r, DisplayPath(path) + L"\r\n  En uso: otro programa impide eliminar este elemento.");
        return true;
    }
    if (error != ERROR_ACCESS_DENIED) { Failure(r, DisplayPath(path) + L" [comprobación de borrado]", error); return false; }
    // Read-only files can reject DELETE even when their security permissions allow it.
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) && (attributes & FILE_ATTRIBUTE_READONLY)) return false;
    const size_t separator = path.find_last_of(L'\\');
    if (separator != std::wstring::npos) {
        std::wstring parent = path.substr(0, separator + 1);
        Handle parentRights{ CreateFileW(parent.c_str(), FILE_DELETE_CHILD,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
        if (parentRights.value != INVALID_HANDLE_VALUE) return false;
    }
    ++r.deleteDenied;
    Failure(r, DisplayPath(path) + L" [no se obtuvo permiso para eliminar el elemento ni desde su carpeta contenedora; puede impedir borrar la carpeta principal]", error);
    return false;
}
bool SameProcess(const RM_UNIQUE_PROCESS& a, const RM_UNIQUE_PROCESS& b) {
    return a.dwProcessId == b.dwProcessId &&
        CompareFileTime(&a.ProcessStartTime, &b.ProcessStartTime) == 0;
}
void CheckDirectoryUsers(Report& r, const std::wstring& path, std::stop_token stop) {
    // Optional native query: information class 47 is reserved for system use.
    // Resolve at runtime and retain the sharing-violation diagnosis if unsupported.
    using Query = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    using ToError = ULONG(NTAPI*)(NTSTATUS);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryInformationFile"));
    auto toError = reinterpret_cast<ToError>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    DWORD error = ERROR_NOT_SUPPORTED;
    bool complete = false;
    if (query) {
        Handle directory{ CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
        if (directory.value == INVALID_HANDLE_VALUE) error = GetLastError();
        else {
            struct ProcessIds { ULONG count; ULONG_PTR ids[1]; };
            for (size_t size = 1024; size <= 1024 * 1024 && !stop.stop_requested(); size *= 2) {
                std::vector<ULONG_PTR> buffer(size / sizeof(ULONG_PTR));
                IO_STATUS_BLOCK io{};
                const NTSTATUS status = query(directory.value, &io, buffer.data(), static_cast<ULONG>(size), 47);
                if (static_cast<ULONG>(status) == 0x80000005 || static_cast<ULONG>(status) == 0xc0000004 ||
                    static_cast<ULONG>(status) == 0xc0000023) continue;
                if (status != 0) { error = toError ? toError(status) : ERROR_NOT_SUPPORTED; break; }
                const size_t offset = offsetof(ProcessIds, ids);
                ULONG count = 0;
                std::memcpy(&count, buffer.data(), sizeof(count));
                if (io.Information < offset || io.Information > size || count > (io.Information - offset) / sizeof(ULONG_PTR)) {
                    error = ERROR_INVALID_DATA; break;
                }
                complete = true;
                bool found = false;
                for (ULONG i = 0; i < count && !stop.stop_requested(); ++i) {
                    ULONG_PTR rawPid = 0;
                    std::memcpy(&rawPid, reinterpret_cast<const BYTE*>(buffer.data()) + offset + i * sizeof(rawPid), sizeof(rawPid));
                    if (!rawPid || rawPid > MAXDWORD || rawPid == GetCurrentProcessId()) continue;
                    const DWORD pid = static_cast<DWORD>(rawPid);
                    Handle process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) };
                    if (!process.value) {
                        const DWORD processError = GetLastError();
                        if (processError == ERROR_INVALID_PARAMETER) continue; // Process already exited.
                        r.needsAdmin |= processError == ERROR_ACCESS_DENIED;
                        Detail(r, DisplayPath(path) + L"\r\n  La carpeta está abierta por PID " + std::to_wstring(pid) +
                            L"; no se pudo consultar su identidad: " + ErrorText(processError));
                        found = true;
                        continue;
                    }
                    FILETIME start{}, exit{}, kernel{}, user{};
                    if (!GetProcessTimes(process.value, &start, &exit, &kernel, &user)) continue;
                    std::wstring executable(32768, L'\0');
                    DWORD length = static_cast<DWORD>(executable.size());
                    if (QueryFullProcessImageNameW(process.value, 0, executable.data(), &length)) executable.resize(length);
                    else executable.clear();
                    std::wstring name = executable.empty() ? L"Proceso" : executable.substr(executable.find_last_of(L"\\/") + 1);
                    RM_UNIQUE_PROCESS identity{ pid, start };
                    Detail(r, DisplayPath(path) + L"\r\n  Carpeta abierta por " + name + L" (PID " + std::to_wstring(pid) + L")." +
                        (executable.empty() ? L"" : L"\r\n  Programa: " + executable));
                    if (std::none_of(r.processes.begin(), r.processes.end(), [&](const Process& p) { return SameProcess(p.identity, identity); }))
                        r.processes.push_back({ identity, name, true });
                    found = true;
                }
                if (found) {
                    ++r.directoriesInUse;
                    Detail(r, DisplayPath(path) + L"\r\n  Uno o varios programas mantienen abierta esta carpeta. La lista incluye candidatos: Windows no indica aquí cuál de sus aperturas impide borrar. Desbloquear te preguntará por cada proceso. También podés cambiar la ubicación de una terminal o cerrar esta carpeta en el Explorador, Codex o el editor y volver a analizar.");
                }
                break;
            }
        }
    }
    if (!complete && !stop.stop_requested()) {
        ++r.directoryQueryErrors;
        r.needsAdmin |= error == ERROR_ACCESS_DENIED;
        Detail(r, DisplayPath(path) + L"\r\n  La carpeta está bloqueada, pero la consulta adicional de procesos no está disponible o no pudo completarse: " + ErrorText(error));
    }
}
std::vector<RM_PROCESS_INFO> Users(std::vector<LPCWSTR> paths, DWORD& error) {
    Session session;
    error = session.error;
    if (error) return {};
    error = RmRegisterResources(session.id, static_cast<UINT>(paths.size()), paths.data(), 0, nullptr, 0, nullptr);
    if (error) return {};
    UINT needed = 0, count = 0;
    DWORD reasons = 0;
    std::vector<RM_PROCESS_INFO> result;
    for (int retry = 0; retry < 5; ++retry) {
        count = static_cast<UINT>(result.size());
        error = RmGetList(session.id, &needed, &count, result.empty() ? nullptr : result.data(), &reasons);
        if (error == ERROR_SUCCESS) { result.resize(count); return result; }
        if (error != ERROR_MORE_DATA) return {};
        result.resize(needed);
    }
    return {};
}
void CheckUsers(Report& r, std::stop_token stop) {
    for (size_t first = 0; first < r.files.size() && !stop.stop_requested(); first += 64) {
        std::vector<LPCWSTR> paths;
        const size_t last = (std::min)(first + 64, r.files.size());
        for (size_t i = first; i < last; ++i)
            if (!r.files[i].metadataOnly) paths.push_back(r.files[i].path.c_str());
        if (paths.empty()) continue;
        DWORD error = 0;
        auto batch = Users(paths, error);
        if (error) {
            Failure(r, L"No se pudo consultar qué programas usan este grupo de archivos", error);
            continue;
        }
        if (batch.empty()) continue;
        for (size_t i = first; i < last && !stop.stop_requested(); ++i) {
            if (r.files[i].metadataOnly) continue;
            auto users = paths.size() == 1 ? batch : Users({ r.files[i].path.c_str() }, error);
            if (error) { Failure(r, DisplayPath(r.files[i].path), error); continue; }
            if (!users.empty()) ++r.inUse;
            for (const auto& user : users) {
                std::wstring name = user.strAppName;
                if (name.empty()) name = L"Proceso";
                Detail(r, DisplayPath(r.files[i].path) + L"\r\n  En uso por " + name +
                    L" (PID " + std::to_wstring(user.Process.dwProcessId) + L").");
                auto existing = std::find_if(r.processes.begin(), r.processes.end(), [&](const Process& p) {
                    return SameProcess(p.identity, user.Process);
                });
                if (existing == r.processes.end()) r.processes.push_back({ user.Process, name });
                else existing->directoryReference = false; // Restart Manager confirmed file usage too.
            }
        }
    }
}
struct CloseContext {
    const std::vector<Process>& approved;
    bool denied = false;
    size_t requested = 0;
};
BOOL CALLBACK CloseWindow(HWND window, LPARAM param) {
    auto& context = *reinterpret_cast<CloseContext*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    for (const auto& process : context.approved) {
        if (pid != process.identity.dwProcessId || pid == GetCurrentProcessId()) continue;
        Handle handle{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) };
        if (!handle.value) { context.denied |= GetLastError() == ERROR_ACCESS_DENIED; continue; }
        FILETIME start{}, exit{}, kernel{}, user{};
        if (handle.value && GetProcessTimes(handle.value, &start, &exit, &kernel, &user) &&
            CompareFileTime(&start, &process.identity.ProcessStartTime) == 0 &&
            IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
            if (PostMessageW(window, WM_CLOSE, 0, 0)) ++context.requested;
            else context.denied |= GetLastError() == ERROR_ACCESS_DENIED;
        }
    }
    return TRUE;
}
}

std::wstring ErrorText(DWORD error) {
    WCHAR* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = buffer ? buffer : L"Error de Windows";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text + L" (" + std::to_wstring(error) + L").";
}
bool IsAdministrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &member);
        FreeSid(group);
    }
    return member != FALSE;
}
Report Analyze(const std::vector<std::wstring>& roots, std::stop_token stop) {
    Report r;
    std::vector<std::wstring> pending;
    for (const auto& root : roots) pending.push_back(FullPath(root));
    std::set<std::wstring, PathLess> seen;
    while (!pending.empty() && !stop.stop_requested()) {
        std::wstring path = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(path).second) continue;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { Failure(r, DisplayPath(path), GetLastError()); continue; }
        bool cloud = false;
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            DWORD error = 0;
            cloud = CloudReparse(path, error);
            if (error) Failure(r, DisplayPath(path) + L" [tipo de punto de redirección]", error);
            if (!cloud) {
            ++r.skipped;
            Detail(r, DisplayPath(path) + L"\r\n  Enlace o punto de redirección omitido.");
            continue;
            }
        }
        if (cloud) {
            ++r.cloudItems;
            Detail(r, DisplayPath(path) + L"\r\n  Elemento administrado por un proveedor de nube, como OneDrive. Este indicador no confirma un bloqueo.");
        }
        const bool deletionBlocked = CheckDeletion(r, path, attributes);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++r.folders;
            if (deletionBlocked) CheckDirectoryUsers(r, path, stop);
            if (attributes & FILE_ATTRIBUTE_READONLY) {
                r.readOnlyFolders.push_back(path);
                Detail(r, DisplayPath(path) + L"\r\n  Carpeta con atributo de solo lectura. Esta marca por sí sola no impide borrar en Windows; se revisan también permisos, contenido y programas en uso.");
            }
            Handle directory{ CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
            if (directory.value == INVALID_HANDLE_VALUE) {
                DWORD error = GetLastError();
                if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
                    ++r.sharingBlocks;
                    Detail(r, DisplayPath(path) + L"\r\n  La carpeta está en uso. Windows puede no informar quién mantiene abierta una carpeta; cerrá la ventana o programa que la usa y volvé a analizar.");
                } else Failure(r, DisplayPath(path) + L" [acceso a la carpeta]", error);
            }
            WIN32_FIND_DATAW data{};
            const std::wstring prefix = path + (path.back() == L'\\' ? L"" : L"\\");
            HANDLE search = FindFirstFileExW((prefix + L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
            if (search == INVALID_HANDLE_VALUE) {
                DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND) Failure(r, DisplayPath(path), error);
                continue;
            }
            do {
                if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) pending.push_back(prefix + data.cFileName);
                if (stop.stop_requested()) break;
            } while (FindNextFileW(search, &data));
            const DWORD error = GetLastError();
            FindClose(search);
            if (!stop.stop_requested() && error != ERROR_NO_MORE_FILES) Failure(r, DisplayPath(path), error);
            continue;
        }
        File file{ path, (attributes & FILE_ATTRIBUTE_READONLY) != 0, false,
            cloud && (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0 };
        if (file.metadataOnly) {
            ++r.metadataOnly;
            Detail(r, DisplayPath(path) + L"\r\n  Archivo de nube no disponible completamente en este equipo. Se revisan atributos y permisos; se omiten la marca de Internet y la consulta de programas en uso.");
        } else {
        Handle stream{ CreateFileW((path + L":Zone.Identifier").c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr) };
        if (stream.value != INVALID_HANDLE_VALUE) file.internet = true;
        else {
            DWORD error = GetLastError();
            if (!Missing(error)) Failure(r, DisplayPath(path) + L" [marca de Internet]", error);
        }
        }
        Handle access{ CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr) };
        if (access.value == INVALID_HANDLE_VALUE) Failure(r, DisplayPath(path), GetLastError());
        // READ_CONTROL checks permission without changing content or taking ownership.
        Handle permissions{ CreateFileW(path.c_str(), READ_CONTROL | FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr) };
        if (permissions.value == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
                Failure(r, DisplayPath(path) + L" [permisos para modificar atributos]", error);
            else if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
                { ++r.sharingBlocks; Detail(r, DisplayPath(path) + L"\r\n  Otro programa impide acceder al archivo."); }
            else Failure(r, DisplayPath(path), error);
        }
        if (file.readOnly) { ++r.readOnly; Detail(r, DisplayPath(path) + L"\r\n  Solo lectura."); }
        if (file.internet) { ++r.internet; Detail(r, DisplayPath(path) + L"\r\n  Marca de archivo descargado de Internet."); }
        r.files.push_back(std::move(file));
    }
    CheckUsers(r, stop);
    r.cancelled = stop.stop_requested();
    return r;
}
Report Unblock(const std::vector<std::wstring>& roots, const std::vector<Process>& approved,
    std::wstring& outcome, std::stop_token stop) {
    Report before = Analyze(roots, stop);
    size_t changed = 0, failed = 0;
    bool denied = false;
    std::vector<std::wstring> failures;
    std::wstring closeOutcome;
    for (const auto& allowed : approved) {
        if (stop.stop_requested()) break;
        const auto stillUsing = std::find_if(before.processes.begin(), before.processes.end(), [&](const Process& current) {
            return SameProcess(current.identity, allowed.identity);
        });
        if (stillUsing == before.processes.end()) continue;
        const auto pid = allowed.identity.dwProcessId;
        const std::wstring label = allowed.name + L" (PID " + std::to_wstring(pid) + L")";
        if (pid == GetCurrentProcessId() || pid <= 4) {
            closeOutcome += label + L": proceso protegido, no se cerró.\r\n";
            continue;
        }
        Handle process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid) };
        if (!process.value) {
            const DWORD error = GetLastError();
            denied |= error == ERROR_ACCESS_DENIED;
            closeOutcome += label + L": no se pudo comprobar antes del cierre. " + ErrorText(error) + L"\r\n";
            continue;
        }
        FILETIME start{}, exit{}, kernel{}, user{};
        BOOL critical = FALSE;
        if (!GetProcessTimes(process.value, &start, &exit, &kernel, &user) ||
            CompareFileTime(&start, &allowed.identity.ProcessStartTime) != 0) continue;
        if (!IsProcessCritical(process.value, &critical) || critical) {
            closeOutcome += label + L": crítico o sin verificación de seguridad, no se cerró.\r\n";
            continue;
        }
        const std::vector<Process> single{ allowed };
        CloseContext close{ single };
        EnumWindows(CloseWindow, reinterpret_cast<LPARAM>(&close));
        denied |= close.denied;
        // User confirmation explicitly covers normal close followed by termination.
        if (close.requested) {
            for (int i = 0; i < 50 && !stop.stop_requested(); ++i)
                if (WaitForSingleObject(process.value, 100) == WAIT_OBJECT_0) break;
        }
        if (stop.stop_requested()) break;
        before = Analyze(roots, stop);
        const bool remains = std::any_of(before.processes.begin(), before.processes.end(), [&](const Process& current) {
            return SameProcess(current.identity, allowed.identity);
        });
        if (!remains || WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) {
            closeOutcome += label + L": ya no figura reteniendo la selección.\r\n";
            continue;
        }
        Handle terminate{ OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid) };
        DWORD error = ERROR_SUCCESS;
        if (!terminate.value) error = GetLastError();
        else if (!GetProcessTimes(terminate.value, &start, &exit, &kernel, &user)) error = GetLastError();
        else if (CompareFileTime(&start, &allowed.identity.ProcessStartTime) != 0) continue;
        else if (!IsProcessCritical(terminate.value, &critical) || critical) {
            closeOutcome += label + L": proceso protegido, no se finalizó.\r\n";
            continue;
        } else if (!TerminateProcess(terminate.value, ERROR_CANCELLED)) error = GetLastError();
        else if (WaitForSingleObject(terminate.value, 5000) != WAIT_OBJECT_0) error = ERROR_TIMEOUT;
        if (error) {
            denied |= error == ERROR_ACCESS_DENIED;
            closeOutcome += label + L": no se pudo finalizar. " + ErrorText(error) + L"\r\n";
        } else closeOutcome += label + L": proceso finalizado con tu autorización.\r\n";
        before = Analyze(roots, stop);
    }
    size_t changedFolders = 0;
    for (const auto& folder : before.readOnlyFolders) {
        if (stop.stop_requested()) break;
        DWORD attrs = GetFileAttributesW(folder.c_str()), error = ERROR_SUCCESS;
        bool cloud = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) && CloudReparse(folder, error);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
            ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && !cloud)) {
            ++failed;
            if (failures.size() < 20) failures.push_back(DisplayPath(folder) + L": la carpeta cambió o ya no está disponible.");
        } else if (SetFileAttributesW(folder.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) ++changedFolders;
        else {
            error = GetLastError();
            denied |= error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
            ++failed;
            if (failures.size() < 20) failures.push_back(DisplayPath(folder) + L": " + ErrorText(error));
        }
    }
    for (const auto& file : before.files) {
        if (stop.stop_requested()) break;
        if (!file.readOnly && !file.internet) continue;
        DWORD attrs = GetFileAttributesW(file.path.c_str());
        DWORD tagError = 0;
        bool cloud = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) && CloudReparse(file.path, tagError);
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) ||
            ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && !cloud)) {
            ++failed;
            if (failures.size() < 20) failures.push_back(DisplayPath(file.path) + L": el archivo cambió o ya no está disponible.");
            continue;
        }
        bool okay = true;
        DWORD error = 0;
        if (file.readOnly && !SetFileAttributesW(file.path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) {
            okay = false; error = GetLastError();
        }
        if (file.internet && !DeleteFileW((file.path + L":Zone.Identifier").c_str())) {
            DWORD streamError = GetLastError();
            if (!Missing(streamError)) { okay = false; error = streamError; }
        }
        if (okay) ++changed;
        else {
            ++failed;
            denied |= error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
            if (failures.size() < 20) failures.push_back(DisplayPath(file.path) + L": " + ErrorText(error));
        }
    }
    auto after = Analyze(roots, stop);
    after.needsAdmin |= denied;
    outcome = L"Desbloqueo: " + std::to_wstring(changed) + L" archivo(s) actualizado(s), " +
        std::to_wstring(changedFolders) + L" carpeta(s) actualizada(s), " + std::to_wstring(failed) + L" con error.\r\n";
    outcome += closeOutcome;
    for (const auto& failure : failures) outcome += failure + L"\r\n";
    return after;
}
std::wstring Describe(const Report& r) {
    std::wostringstream text;
    text << r.files.size() << L" archivo(s) · " << r.folders << L" carpeta(s)\r\n\r\n";
    text << L"Marca de Internet: " << r.internet << L"\r\nSolo lectura: " << r.readOnly
        << L" archivo(s)\r\nCarpetas con marca de solo lectura: " << r.readOnlyFolders.size()
        << L"\r\nElementos sin permiso de borrado: " << r.deleteDenied
        << L"\r\nArchivos en uso: " << r.inUse << L"\r\nBloqueos de acceso: " << r.sharingBlocks
        << L"\r\nErrores de análisis: " << r.errors << L"\r\n";
    if (r.directoriesInUse) text << L"Carpetas abiertas con procesos identificados: " << r.directoriesInUse << L"\r\n";
    if (r.directoryQueryErrors) text << L"Consultas de procesos de carpeta incompletas: " << r.directoryQueryErrors << L"\r\n";
    if (r.skipped) text << L"Enlaces omitidos: " << r.skipped << L"\r\n";
    if (r.cloudItems) text << L"Elementos de nube / OneDrive: " << r.cloudItems << L"\r\n";
    if (r.cloudErrors) text << L"Errores informados por el proveedor de nube: " << r.cloudErrors << L"\r\n";
    if (r.metadataOnly) text << L"Archivos de nube con análisis parcial: " << r.metadataOnly << L"\r\n";
    if (!r.processes.empty()) {
        text << L"\r\nProcesos identificados (" << r.processes.size() << L"):\r\n";
        for (const auto& process : r.processes)
            text << L"• " << process.name << L" — PID " << process.identity.dwProcessId
                << (process.directoryReference ? L" [carpeta abierta; candidato]" : L" [archivo en uso]") << L"\r\n";
    }
    if (r.cancelled) text << L"\r\nAnálisis cancelado.\r\n";
    else if (!r.actionable() && !r.errors && !r.skipped && !r.sharingBlocks && !r.metadataOnly)
        text << L"\r\nNo se detectaron bloqueos en las comprobaciones realizadas.\r\n";
    if (r.needsAdmin) text << L"\r\nFaltan permisos. Podés volver a analizar como administrador.\r\n";
    if (!r.readOnlyFolders.empty()) text << L"\r\nDesbloquear también quitará la marca de solo lectura de las carpetas detectadas. Los permisos de borrado se informan por separado.\r\n";
    if (r.canClosePrograms()) text << L"\r\nDesbloquear preguntará por cada proceso: Sí para autorizar su cierre y posible finalización; No para dejarlo abierto; Cancelar para no iniciar la operación. Guardá tus cambios antes de aceptar.\r\n";
    if (r.directoriesInUse) text << L"\r\nLas aperturas de carpetas incluyen candidatos; Windows no permite distinguir aquí cuál impide el borrado. Solo se actúa sobre los procesos que autorices.\r\n";
    if (r.sharingBlocks && r.processes.empty()) text << L"\r\nNo se pudo identificar el programa responsable del bloqueo. Cerralo manualmente y volvé a analizar.\r\n";
    text << L"\r\n";
    for (const auto& detail : r.details) text << detail << L"\r\n\r\n";
    if (r.details.size() == 200) text << L"Se muestran los primeros 200 detalles; se analizó toda la selección.\r\n";
    text << L"\r\nEl análisis de uso depende de la información que Windows puede consultar. "
        L"Los permisos especiales, servicios y bloqueos del sistema pueden requerir una acción manual.";
    return text.str();
}
}
