#include "auth.h"

#include <security/pam_appl.h>

#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

// PAM conversation: hand the stored password to every echo-off/on prompt.
int converse(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
    if (n <= 0 || !resp)
        return PAM_CONV_ERR;
    const auto *pw = static_cast<const std::string *>(data);
    auto *replies = static_cast<pam_response *>(std::calloc(size_t(n), sizeof(pam_response)));
    if (!replies)
        return PAM_BUF_ERR;
    for (int i = 0; i < n; ++i) {
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            replies[i].resp = strdup(pw ? pw->c_str() : "");
            break;
        default:
            replies[i].resp = nullptr;
            break;
        }
    }
    *resp = replies;
    return PAM_SUCCESS;
}

std::string currentUser()
{
    if (const char *u = std::getenv("USER"))
        return u;
    if (const passwd *pw = getpwuid(getuid()))
        return pw->pw_name;
    return {};
}

} // namespace

Authenticator::Authenticator()
{
    m_eventFd = eventfd(0, EFD_CLOEXEC);
    if (const char *s = std::getenv("VANTALOCK_PAM_SERVICE"))
        m_service = s;
    else
        m_service = "vantalock";
    m_user = currentUser();
}

Authenticator::~Authenticator()
{
    if (m_thread.joinable())
        m_thread.join();
    if (m_eventFd >= 0)
        close(m_eventFd);
}

void Authenticator::authenticate(const std::string &password)
{
    if (m_busy.load())
        return;
    if (m_thread.joinable())
        m_thread.join(); // reap a previous (already-consumed) run
    m_busy.store(true);
    m_success.store(false);
    m_thread = std::thread(&Authenticator::run, this, password);
}

void Authenticator::run(std::string password)
{
    const std::string pw = std::move(password);
    pam_conv conv{ converse, const_cast<void *>(static_cast<const void *>(&pw)) };
    pam_handle_t *h = nullptr;

    int r = pam_start(m_service.c_str(), m_user.c_str(), &conv, &h);
    if (r == PAM_SUCCESS)
        r = pam_authenticate(h, 0);
    if (r == PAM_SUCCESS)
        r = pam_acct_mgmt(h, 0);
    if (h)
        pam_end(h, r);

    m_success.store(r == PAM_SUCCESS);
    m_busy.store(false);

    if (m_eventFd >= 0) {
        const uint64_t one = 1;
        ssize_t w = write(m_eventFd, &one, sizeof(one));
        (void)w;
    }
}

bool Authenticator::consumeResult()
{
    if (m_eventFd >= 0) {
        uint64_t v = 0;
        ssize_t r = read(m_eventFd, &v, sizeof(v));
        (void)r;
    }
    if (m_thread.joinable())
        m_thread.join();
    return m_success.load();
}
