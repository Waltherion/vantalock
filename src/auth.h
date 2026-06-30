#pragma once

#include <atomic>
#include <string>
#include <thread>

// Overwrite a string's bytes (volatile, so the compiler can't elide it) and clear it,
// so a plaintext password doesn't linger in freed memory after a PAM check.
void secureZero(std::string &s);

// Background PAM authentication. PAM can block (modules, the ~2s failure delay),
// so the check runs on a worker thread and signals completion via an eventfd that
// the main Wayland loop polls. The session never unlocks unless consumeResult()
// returns true, so a crash or error keeps the session locked.
class Authenticator
{
public:
    Authenticator();
    ~Authenticator();

    int eventFd() const { return m_eventFd; }
    bool busy() const { return m_busy.load(); }

    // Spawn a background PAM check for the given password. No-op while busy.
    void authenticate(const std::string &password);

    // Call once the eventfd is readable: drains it, joins the worker, and returns
    // true iff authentication succeeded.
    bool consumeResult();

private:
    void run(std::string password);

    int m_eventFd = -1;
    std::thread m_thread;
    std::atomic<bool> m_busy{ false };
    std::atomic<bool> m_success{ false };
    std::string m_service;
    std::string m_user;
};
