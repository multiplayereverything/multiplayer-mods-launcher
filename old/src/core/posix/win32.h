// The small part of the Win32 API the engine uses, done with POSIX calls (posix.cpp), so the shared code builds on
// Linux unchanged. Paths may use '\' or '/': both reach the OS as '/', and a path that does not exist as written is
// looked up ignoring case, the way Windows (and Proton) games expect. Only what the engine calls is here; process
// creation, HTTP, the registry, known folders and the console have Linux twins next to their Windows code instead.
#pragma once
#ifndef _WIN32

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <type_traits>

// ---- types ------------------------------------------------------------------------------------------------------
using BYTE = uint8_t;
using UCHAR = uint8_t;
using PUCHAR = uint8_t*;
using WORD = uint16_t;
using USHORT = uint16_t;
using DWORD = uint32_t;
using UINT = uint32_t;
using ULONG = uint32_t;
using u_long = unsigned long;
using LONG = int32_t;
using BOOL = int;
using INT_PTR = intptr_t;
using ULONGLONG = uint64_t;
using LONGLONG = int64_t;
using HRESULT = int32_t;
using WCHAR = wchar_t;
using PWSTR = wchar_t*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using LPVOID = void*;
using HANDLE = void*;
using HINSTANCE = void*;
using HMODULE = void*;
using HKEY = void*;
using HWND = void*;
using SOCKET = int;

#define WINAPI
#define CALLBACK
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#define S_OK 0
#define MAKEWORD(a, b) static_cast<WORD>((static_cast<BYTE>(a)) | (static_cast<WORD>(static_cast<BYTE>(b)) << 8))

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
};
union ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
};
struct FILETIME {
    DWORD dwLowDateTime, dwHighDateTime;
};
struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    void* lpSecurityDescriptor;
    BOOL bInheritHandle;
};
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
};
struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwReserved0, dwReserved1;
    wchar_t cFileName[MAX_PATH * 4];
    wchar_t cAlternateFileName[14];
};
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };

// ---- errors (the Win32 codes the engine checks) ---------------------------------------------------------------------
#define ERROR_SUCCESS 0L
#define NO_ERROR 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_NOT_READY 21L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_HANDLE_DISK_FULL 39L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_DISK_FULL 112L
#define ERROR_BUFFER_OVERFLOW 111L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_DIRECTORY 267L
#define ERROR_NOT_SAME_DEVICE 17L
#define ERROR_USER_MAPPED_FILE 1224L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_TIMEOUT 1460L
DWORD GetLastError();
void SetLastError(DWORD code);
// errno to the closest Win32 code (EACCES to 5, ENOENT to 2, ...).
DWORD flWin32FromErrno(int e);

// ---- files ------------------------------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_HIDDEN 0x2u
#define FILE_ATTRIBUTE_SYSTEM 0x4u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_ATTRIBUTE_TEMPORARY 0x100u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_COPY_ALLOWED 0x2u
#define MOVEFILE_WRITE_THROUGH 0x8u
#define FILE_TYPE_UNKNOWN 0u
#define FILE_TYPE_DISK 1u
#define FILE_TYPE_CHAR 2u
#define FILE_TYPE_PIPE 3u
#define STD_INPUT_HANDLE (static_cast<DWORD>(-10))
#define STD_OUTPUT_HANDLE (static_cast<DWORD>(-11))
#define STD_ERROR_HANDLE (static_cast<DWORD>(-12))

// The OS form of a Windows-style path: '/' separators, and the real spelling of any part whose case differs.
std::string flOsPath(const wchar_t* path);
std::string flOsPath(const std::wstring& path);

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa, DWORD disposition, DWORD flags,
                   HANDLE templ);
BOOL ReadFile(HANDLE h, void* buf, DWORD size, DWORD* read, void* overlapped);
BOOL WriteFile(HANDLE h, const void* buf, DWORD size, DWORD* written, void* overlapped);
BOOL FlushFileBuffers(HANDLE h);
BOOL CloseHandle(HANDLE h);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
DWORD GetFileType(HANDLE h);
BOOL PeekNamedPipe(HANDLE h, void* buf, DWORD size, DWORD* read, DWORD* avail, DWORD* left);
HANDLE GetStdHandle(DWORD which);
BOOL SetStdHandle(DWORD which, HANDLE h);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void* out);
BOOL SetFileAttributesW(LPCWSTR path, DWORD attrs);
BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES* sa);
BOOL RemoveDirectoryW(LPCWSTR path);
BOOL DeleteFileW(LPCWSTR path);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* out);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out);
BOOL FindClose(HANDLE h);
DWORD GetTempPathW(DWORD size, wchar_t* buf);
DWORD GetModuleFileNameW(HMODULE module, wchar_t* buf, DWORD size);
DWORD GetFullPathNameW(LPCWSTR path, DWORD size, wchar_t* buf, wchar_t** filePart);
BOOL GetDiskFreeSpaceExW(LPCWSTR dir, ULARGE_INTEGER* avail, ULARGE_INTEGER* total, ULARGE_INTEGER* free);
DWORD SearchPathW(LPCWSTR path, LPCWSTR name, LPCWSTR ext, DWORD size, wchar_t* buf, wchar_t** filePart);

// ---- events, processes, waiting ----------------------------------------------------------------------------------------
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS 64u
#define STILL_ACTIVE 259u
HANDLE CreateEventW(SECURITY_ATTRIBUTES* sa, BOOL manualReset, BOOL initialState, LPCWSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms);
// A waitable handle for a process this one started (exit code from waitpid) or any other running one.
HANDLE flProcessHandle(pid_t pid);
pid_t flProcessId(HANDLE process);
// A file handle over an open descriptor (a pipe end, say); CloseHandle closes it.
HANDLE flFileHandle(int fd);
BOOL GetExitCodeProcess(HANDLE h, DWORD* code);
BOOL TerminateProcess(HANDLE h, UINT code);
DWORD GetCurrentProcessId();
void Sleep(DWORD ms);
ULONGLONG GetTickCount64();
void GetSystemTime(SYSTEMTIME* out);
void GetLocalTime(SYSTEMTIME* out);

// ---- environment, text --------------------------------------------------------------------------------------------
#define CP_UTF8 65001u
#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100u
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x200u
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000u
DWORD GetEnvironmentVariableW(LPCWSTR name, wchar_t* buf, DWORD size);
DWORD GetEnvironmentVariableA(const char* name, char* buf, DWORD size);
wchar_t* GetEnvironmentStringsW();
BOOL FreeEnvironmentStringsW(wchar_t* block);
int MultiByteToWideChar(UINT cp, DWORD flags, const char* s, int n, wchar_t* out, int outSize);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* s, int n, char* out, int outSize, const char* def,
                        BOOL* usedDef);
DWORD CharLowerBuffW(wchar_t* s, DWORD n);
DWORD CharUpperBuffW(wchar_t* s, DWORD n);
int CompareStringOrdinal(const wchar_t* a, int na, const wchar_t* b, int nb, BOOL ignoreCase);
DWORD FormatMessageW(DWORD flags, const void* source, DWORD code, DWORD lang, wchar_t* buf, DWORD size, void* args);
void* LocalFree(void* p);
wchar_t* GetCommandLineW();
wchar_t** CommandLineToArgvW(const wchar_t* line, int* argc);  // freed with LocalFree
long BCryptGenRandom(void* alg, PUCHAR buf, ULONG size, ULONG flags);
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 2u

// _wtoi, _wcsicmp and the other C runtime names: crt.h (every source file gets it, as MSVC's headers give them).
#include "crt.h"
FILE* _wfopen(const wchar_t* path, const wchar_t* mode);
inline FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int) { return _wfopen(path, mode); }
inline int _wfopen_s(FILE** f, const wchar_t* path, const wchar_t* mode) {
    *f = _wfopen(path, mode);
    return *f ? 0 : errno;
}
#define _SH_DENYNO 0x40
#define _SH_DENYWR 0x20

// ---- sockets (BSD sockets already are Winsock's model) -----------------------------------------------------------------
struct WSADATA {
    WORD wVersion;
};
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#define SD_BOTH SHUT_RDWR
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAETIMEDOUT ETIMEDOUT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEACCES EACCES
#define WSAECONNRESET ECONNRESET
#define WSAEINTR EINTR
inline int WSAStartup(WORD, WSADATA* d) {
    if (d) d->wVersion = MAKEWORD(2, 2);
    return 0;
}
inline int WSACleanup() { return 0; }
inline int WSAGetLastError() { return errno; }
inline int closesocket(SOCKET s) { return close(s); }
int ioctlsocket(SOCKET s, long cmd, u_long* arg);
const wchar_t* InetNtopW(int family, const void* addr, wchar_t* buf, size_t size);
// Winsock's exclusive bind is what Linux does by default; SO_REUSEADDR only lets a port in TIME_WAIT be bound again.
#define SO_EXCLUSIVEADDRUSE SO_REUSEADDR
// Winsock takes timeouts as a DWORD of milliseconds and ignores select's first argument; Linux wants a timeval and the
// highest descriptor + 1.
int flSetsockopt(SOCKET s, int level, int name, const void* value, socklen_t size);
#define setsockopt(s, level, name, value, size) flSetsockopt(s, level, name, value, static_cast<socklen_t>(size))
inline bool flFdSetEmpty(const fd_set* s) {  // Winsock's fd_count == 0
    for (int fd = 0; fd < FD_SETSIZE; ++fd)
        if (FD_ISSET(fd, s)) return false;
    return true;
}
inline int flSelect(int n, fd_set* r, fd_set* w, fd_set* e, timeval* t) { return ::select(n > 0 ? n : FD_SETSIZE, r, w, e, t); }
#define select(n, r, w, e, t) flSelect(n, r, w, e, t)
// Winsock's lengths are int; with nullptr the plain call is used.
template <class T>
inline int accept(int s, sockaddr* a, T* len) requires(sizeof(T) == sizeof(socklen_t) && !std::is_same_v<T, socklen_t>) {
    return ::accept(s, a, reinterpret_cast<socklen_t*>(len));
}
template <class T>
inline int getsockname(int s, sockaddr* a, T* len) requires(sizeof(T) == sizeof(socklen_t) && !std::is_same_v<T, socklen_t>) {
    return ::getsockname(s, a, reinterpret_cast<socklen_t*>(len));
}
template <class T>
inline int getpeername(int s, sockaddr* a, T* len) requires(sizeof(T) == sizeof(socklen_t) && !std::is_same_v<T, socklen_t>) {
    return ::getpeername(s, a, reinterpret_cast<socklen_t*>(len));
}
template <class T>
inline ssize_t recvfrom(int s, void* b, size_t n, int f, sockaddr* a, T* len) requires(sizeof(T) == sizeof(socklen_t) && !std::is_same_v<T, socklen_t>) {
    return ::recvfrom(s, b, n, f, a, reinterpret_cast<socklen_t*>(len));
}

#endif
