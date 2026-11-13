#include <Portal.h>
#include <csignal>

/**
 * xdg-desktop-portal-cz: the Cuarzo portal backend (org.freedesktop.impl.portal.desktop.cz).
 *
 * Implements org.freedesktop.impl.portal.ScreenCast with Louvre's (and any compositor's) capture
 * protocols: ext-image-capture-source, ext-image-copy-capture and ext-foreign-toplevel-list.
 */
int main()
{
    // A consumer going away mid write must not kill us
    signal(SIGPIPE, SIG_IGN);

    auto &portal { Portal::Get() };

    if (!portal.init())
        return 1;

    return portal.run();
}
