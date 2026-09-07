#include "platform/CredentialStore.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

namespace {
std::wstring toWide(const QString& s) { return s.toStdWString(); }
}  // namespace

void CredentialStore::save(const QString& target, const QString& value) {
    if (target.isEmpty()) return;
    if (value.isEmpty()) {
        erase(target);
        return;
    }

    const std::wstring wTarget = toWide(target);
    const std::wstring wValue  = toWide(value);

    CREDENTIALW cred = {};
    cred.Type               = CRED_TYPE_GENERIC;
    cred.TargetName         = const_cast<LPWSTR>(wTarget.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(wValue.size() * sizeof(wchar_t));
    cred.CredentialBlob     = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(wValue.data()));
    cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName           = const_cast<LPWSTR>(L"MalloyStudio");
    CredWriteW(&cred, 0);
}

QString CredentialStore::load(const QString& target) {
    if (target.isEmpty()) return QString();

    const std::wstring wTarget = toWide(target);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(wTarget.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
        return QString();

    QString value;
    if (cred->CredentialBlob && cred->CredentialBlobSize > 0) {
        // Stored as UTF-16 LE without a trailing NUL.
        value = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(cred->CredentialBlob),
            static_cast<int>(cred->CredentialBlobSize / sizeof(wchar_t)));
    }
    CredFree(cred);
    return value;
}

void CredentialStore::erase(const QString& target) {
    if (target.isEmpty()) return;
    const std::wstring wTarget = toWide(target);
    CredDeleteW(wTarget.c_str(), CRED_TYPE_GENERIC, 0);
}
