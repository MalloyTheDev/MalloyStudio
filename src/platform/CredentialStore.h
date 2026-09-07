#pragma once

// Windows Credential Manager, wrapped so secrets have one storage path.
//
// The stream key already lived here; OAuth refresh tokens are the same kind of
// secret and belong in the same place rather than in QSettings. Note what this
// does and does not buy you: it keeps secrets out of plain configuration files
// and off disk in cleartext, but any code running as this user can read them
// back with no prompt, so it is not a boundary against local code.

#include <QString>

namespace CredentialStore {

// Stores `value` under `target`, replacing anything already there. An empty
// value erases the credential instead, so callers do not need a separate call
// to clear one.
void save(const QString& target, const QString& value);

// Returns an empty string when nothing is stored under `target`.
QString load(const QString& target);

void erase(const QString& target);

}  // namespace CredentialStore
