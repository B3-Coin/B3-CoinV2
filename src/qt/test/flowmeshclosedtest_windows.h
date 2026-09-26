// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef B3_QT_TEST_FLOWMESHCLOSEDTEST_WINDOWS_H
#define B3_QT_TEST_FLOWMESHCLOSEDTEST_WINDOWS_H

// Private implementation of the portable launcher's Windows storage guard.
// Handles inspect the object actually opened, without following reparse points.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <QByteArray>
#include <QDir>
#include <QString>

namespace FlowMeshClosedTest {
namespace WindowsStorage {
class Handle {
    HANDLE value;
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { if (valid()) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
    HANDLE get() const { return value; }
    HANDLE release() { const HANDLE result{value}; value = INVALID_HANDLE_VALUE; return result; }
};

class PrivateSecurity {
    QByteArray token_user;
    PACL acl{nullptr};
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};
    bool initialized{false};
public:
    PrivateSecurity()
    {
        HANDLE raw_token{nullptr};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return;
        Handle token{raw_token};
        DWORD size{0};
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        if (size == 0 || size > 65536) return;
        token_user.resize(size);
        if (!GetTokenInformation(token.get(), TokenUser, token_user.data(), size, &size)) return;
        // Core-created wallet/settings files use the token's default owner.
        // Refuse an elevated token whose default owner is another SID before
        // creating data that would fail same-user checks on the next launch.
        DWORD owner_size{0};
        GetTokenInformation(token.get(), TokenOwner, nullptr, 0, &owner_size);
        if (owner_size == 0 || owner_size > 65536) return;
        QByteArray token_owner(owner_size, '\0');
        if (!GetTokenInformation(token.get(), TokenOwner, token_owner.data(), owner_size, &owner_size) ||
            !EqualSid(sid(), reinterpret_cast<const TOKEN_OWNER*>(token_owner.constData())->Owner)) return;
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid());
        if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS ||
            !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor, sid(), FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) return;
        attributes = {sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
        initialized = true;
    }
    ~PrivateSecurity() { if (acl) LocalFree(acl); }
    PrivateSecurity(const PrivateSecurity&) = delete;
    PrivateSecurity& operator=(const PrivateSecurity&) = delete;
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(token_user.constData())->User.Sid; }
    bool valid() const { return initialized; }
    SECURITY_ATTRIBUTES* get() { return &attributes; }
};

inline LPCWSTR Wide(const QString& path) { return reinterpret_cast<LPCWSTR>(path.utf16()); }

inline bool PrivateObject(HANDLE handle, bool directory, const PrivateSecurity& security, quint64* size = nullptr)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!security.valid() || GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1)) return false;
    PSID owner{nullptr}; PACL dacl{nullptr}; PSECURITY_DESCRIPTOR descriptor{nullptr};
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    bool safe{owner && IsValidSid(owner) && EqualSid(owner, security.sid()) && dacl && IsValidAcl(dacl)};
    // SYSTEM and administrators are trusted OS principals. No other user's or
    // broad group's grant is accepted, including inherit-only grants.
    bool private_inheritance{!directory};
    if (safe) {
        for (DWORD i{0}; i < dacl->AceCount; ++i) {
            void* raw{nullptr};
            if (!GetAce(dacl, i, &raw)) { safe = false; break; }
            const auto* header{static_cast<const ACE_HEADER*>(raw)};
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceSize < sizeof(ACCESS_ALLOWED_ACE)) { safe = false; break; }
            const auto* ace{static_cast<const ACCESS_ALLOWED_ACE*>(raw)};
            PSID trustee{const_cast<DWORD*>(&ace->SidStart)};
            if (!IsValidSid(trustee) || (!EqualSid(trustee, security.sid()) &&
                !IsWellKnownSid(trustee, WinLocalSystemSid) && !IsWellKnownSid(trustee, WinBuiltinAdministratorsSid))) {
                safe = false; break;
            }
            if (directory && EqualSid(trustee, security.sid()) &&
                (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) &&
                !(header->AceFlags & (INHERIT_ONLY_ACE | NO_PROPAGATE_INHERIT_ACE)) &&
                ((ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS || (ace->Mask & GENERIC_ALL))) private_inheritance = true;
        }
    }
    LocalFree(descriptor);
    safe = safe && private_inheritance;
    if (safe && size) *size = (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return safe;
}

inline HANDLE Open(const QString& path, DWORD access, DWORD sharing, DWORD disposition, SECURITY_ATTRIBUTES* security = nullptr)
{
    return CreateFileW(Wide(QDir::toNativeSeparators(path)), access | READ_CONTROL, sharing, security, disposition,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

inline bool PlainLocalAncestors(const QString& path)
{
    // Do not accept UNC/device namespaces or drive-relative/alternate-stream
    // paths. Inspect every ancestor: Qt's canonical path alone is insufficient
    // for junctions and other Windows reparse-point types.
    if (path.size() < 3 || !path.at(0).isLetter() || path.mid(1, 2) != QStringLiteral(":/") ||
        path.mid(2).contains(QLatin1Char(':')) || path.contains(QLatin1Char('\\'))) return false;
    QString current{path.left(3)};
    const auto parts{path.mid(3).split(QLatin1Char('/'), Qt::SkipEmptyParts)};
    for (int i{-1}; i < parts.size(); ++i) {
        if (i >= 0) {
            if (parts.at(i).endsWith(QLatin1Char('.')) || parts.at(i).endsWith(QLatin1Char(' '))) return false;
            current = QDir{current}.filePath(parts.at(i));
        }
        Handle handle{Open(current, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING)};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!handle.valid() || !GetFileInformationByHandle(handle.get(), &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}

inline bool Read(HANDLE handle, quint64 size, QByteArray& bytes)
{
    if (size > 65536) return false;
    bytes.resize(size);
    DWORD done{0};
    while (done < size) {
        DWORD count{0};
        if (!ReadFile(handle, bytes.data() + done, DWORD(size - done), &count, nullptr) || count == 0) return false;
        done += count;
    }
    return true;
}
} // namespace WindowsStorage
} // namespace FlowMeshClosedTest
#endif
