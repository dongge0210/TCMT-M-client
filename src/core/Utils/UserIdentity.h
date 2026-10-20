// UserIdentity.h — who is running this process, and who invoked it.
//
// Under sudo the effective user is root while the invoking account survives
// in SUDO_USER / SUDO_UID. Anything that reasons about permissions (Location
// Services, privileged helpers) needs the login account, not a bare "root":
// the TUI shows it in the status row and the WiFi panel names it when the
// platform cannot grant a root process what it needs.
//
// Header-only on purpose: the identity is one getpwuid lookup, and this keeps
// the Windows project files from needing another source entry.

#pragma once

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <pwd.h>
#include <cstdlib>
#endif

namespace tcmt {

struct UserIdentity {
    std::string user;       // login user that invoked the process
    std::string effective;  // account the process actually runs as
    bool isRoot = false;    // POSIX: effective uid is 0 / Windows: elevated
    bool viaSudo = false;   // root, but invoked by a normal login user
};

inline UserIdentity GetUserIdentity() {
    UserIdentity id;
#ifdef _WIN32
    char buf[256] = {0};
    DWORD len = static_cast<DWORD>(sizeof(buf));
    if (GetUserNameA(buf, &len)) id.user = buf;
    id.effective = id.user;
#else
    auto nameOf = [](uid_t uid) -> std::string {
        struct passwd* pw = getpwuid(uid);
        return (pw != nullptr && pw->pw_name != nullptr) ? std::string(pw->pw_name)
                                                         : std::string();
    };
    const uid_t euid = geteuid();
    id.isRoot = (euid == 0);
    id.effective = nameOf(euid);
    if (id.effective.empty()) id.effective = std::to_string(static_cast<unsigned>(euid));

    const char* sudoUser = std::getenv("SUDO_USER");
    if (id.isRoot && sudoUser != nullptr && sudoUser[0] != '\0') {
        id.viaSudo = true;
        id.user = sudoUser;
    } else {
        id.user = nameOf(getuid());
        if (id.user.empty()) id.user = id.effective;
    }
#endif
    return id;
}

} // namespace tcmt
