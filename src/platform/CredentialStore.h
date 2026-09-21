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
//
// Returns false when nothing was stored: the target is empty, or Windows
// refused the write (the Credential Manager is unavailable, a policy blocks
// it, or the value is larger than a credential holds). Whatever was stored
// before is then still there. The failure is logged with the target and the
// Windows error, never with the value.
bool save(const QString& target, const QString& value);

// Returns an empty string when nothing is stored under `target`.
QString load(const QString& target);

// Returns true when nothing is stored under `target` afterwards, which
// includes there having been nothing to erase. False for an empty target and
// when Windows refused, which is logged like a failed save.
bool erase(const QString& target);

}  // namespace CredentialStore
