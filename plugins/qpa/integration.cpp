/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2015 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "integration.h"
#include "backingstore.h"
#include "nativeinterface.h"
#include "offscreensurface.h"
#include "screen.h"
#include "sharingplatformcontext.h"
#include "window.h"
#include "../../main.h"
#include "../../platform.h"
#include "../../screens.h"
#include "../../virtualkeyboard.h"
#include "../../wayland_server.h"

#include <KWayland/Client/compositor.h>
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/registry.h>
#include <KWayland/Client/shell.h>
#include <KWayland/Client/surface.h>
#include <KWayland/Server/clientconnection.h>

#include <QCoreApplication>
#include <QtConcurrentRun>

#include <qpa/qplatforminputcontext.h>
#include <qpa/qplatforminputcontextfactory_p.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

#include <QtEventDispatcherSupport/private/qunixeventdispatcher_qpa_p.h>
#include <QtFontDatabaseSupport/private/qgenericunixfontdatabase_p.h>
#include <QtThemeSupport/private/qgenericunixthemes_p.h>

namespace KWin
{

namespace QPA
{

Integration::Integration()
    : QObject()
    , QPlatformIntegration()
    , m_fontDb(new QGenericUnixFontDatabase())
    , m_nativeInterface(new NativeInterface(this))
    , m_inputContext()
{
}

Integration::~Integration() = default;

bool Integration::hasCapability(Capability cap) const
{
    switch (cap) {
    case ThreadedPixmaps:
        return true;
    case OpenGL:
        return true;
    case ThreadedOpenGL:
        return false;
    case BufferQueueingOpenGL:
        return false;
    case MultipleWindows:
    case NonFullScreenWindows:
        return true;
    case RasterGLSurface:
        return false;
    default:
        return QPlatformIntegration::hasCapability(cap);
    }
}

void Integration::initialize()
{
    connect(kwinApp(), &Application::screensCreated, this,
        [this] {
            connect(screens(), &Screens::changed, this, &Integration::initScreens);
            initScreens();
        }
    );
    QPlatformIntegration::initialize();
    auto dummyScreen = new Screen(-1);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
    QWindowSystemInterface::handleScreenAdded(dummyScreen);
#else
    screenAdded(dummyScreen);
#endif
    m_screens << dummyScreen;
    m_inputContext.reset(QPlatformInputContextFactory::create(QStringLiteral("qtvirtualkeyboard")));
    qunsetenv("QT_IM_MODULE");
    if (!m_inputContext.isNull()) {
        connect(qApp, &QGuiApplication::focusObjectChanged, this,
            [this] {
                if (VirtualKeyboard::self() && qApp->focusObject() != VirtualKeyboard::self()) {
                    m_inputContext->setFocusObject(VirtualKeyboard::self());
                }
            }
        );
        connect(kwinApp(), &Application::workspaceCreated, this,
            [this] {
                if (VirtualKeyboard::self()) {
                    m_inputContext->setFocusObject(VirtualKeyboard::self());
                }
            }
        );
        connect(qApp->inputMethod(), &QInputMethod::visibleChanged, this,
            [] {
                if (qApp->inputMethod()->isVisible()) {
                    if (QWindow *w = VirtualKeyboard::self()->inputPanel()) {
                        QWindowSystemInterface::handleWindowActivated(w, Qt::ActiveWindowFocusReason);
                    }
                }
            }
        );
    }
}

QAbstractEventDispatcher *Integration::createEventDispatcher() const
{
    return new QUnixEventDispatcherQPA;
}

QPlatformBackingStore *Integration::createPlatformBackingStore(QWindow *window) const
{
    auto registry = waylandServer()->internalClientRegistry();
    const auto shm = registry->interface(KWayland::Client::Registry::Interface::Shm);
    if (shm.name == 0u) {
        return nullptr;
    }
    return new BackingStore(window, registry->createShmPool(shm.name, shm.version, window));
}

QPlatformWindow *Integration::createPlatformWindow(QWindow *window) const
{
    auto c = compositor();
    auto s = shell();
    if (!s || !c) {
        return new QPlatformWindow(window);
    } else {
        // don't set window as parent, cause infinite recursion in PlasmaQuick::Dialog
        auto surface = c->createSurface(c);
        return new Window(window, surface, s->createSurface(surface, surface), this);
    }
}

QPlatformOffscreenSurface *Integration::createPlatformOffscreenSurface(QOffscreenSurface *surface) const
{
    return new OffscreenSurface(surface);
}

QPlatformFontDatabase *Integration::fontDatabase() const
{
    return m_fontDb;
}

QPlatformTheme *Integration::createPlatformTheme(const QString &name) const
{
    return QGenericUnixTheme::createUnixTheme(name);
}

QStringList Integration::themeNames() const
{
    if (qEnvironmentVariableIsSet("KDE_FULL_SESSION")) {
        return QStringList({QStringLiteral("kde")});
    }
    return QStringList({QLatin1String(QGenericUnixTheme::name)});
}

QPlatformNativeInterface *Integration::nativeInterface() const
{
    return m_nativeInterface;
}

QPlatformOpenGLContext *Integration::createPlatformOpenGLContext(QOpenGLContext *context) const
{
    if (kwinApp()->platform()->supportsQpaContext()) {
        return new SharingPlatformContext(context);
    }
    if (kwinApp()->platform()->sceneEglDisplay() != EGL_NO_DISPLAY) {
        auto s = kwinApp()->platform()->sceneEglSurface();
        if (s != EGL_NO_SURFACE) {
            // try a SharingPlatformContext with a created surface
            return new SharingPlatformContext(context, s, kwinApp()->platform()->sceneEglConfig());
        }
    }
    return nullptr;
}

void Integration::initScreens()
{
    QVector<Screen*> newScreens;
    newScreens.reserve(qMax(screens()->count(), 1));
    for (int i = 0; i < screens()->count(); i++) {
        auto screen = new Screen(i);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
        QWindowSystemInterface::handleScreenAdded(screen);
#else
        screenAdded(screen);
#endif
        newScreens << screen;
    }
    if (newScreens.isEmpty()) {
        auto dummyScreen = new Screen(-1);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
        QWindowSystemInterface::handleScreenAdded(dummyScreen);
#else
        screenAdded(dummyScreen);
#endif
        newScreens << dummyScreen;
    }
    while (!m_screens.isEmpty()) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
        QWindowSystemInterface::handleScreenRemoved(m_screens.takeLast());
#else
        destroyScreen(m_screens.takeLast());
#endif
    }
    m_screens = newScreens;
}

KWayland::Client::Compositor *Integration::compositor() const
{
    if (!m_compositor) {
        using namespace KWayland::Client;
        auto registry = waylandServer()->internalClientRegistry();
        const auto c = registry->interface(Registry::Interface::Compositor);
        if (c.name != 0u) {
            const_cast<Integration*>(this)->m_compositor = registry->createCompositor(c.name, c.version, registry);
        }
    }
    return m_compositor;
}

KWayland::Client::Shell *Integration::shell() const
{
    if (!m_shell) {
        using namespace KWayland::Client;
        auto registry = waylandServer()->internalClientRegistry();
        const auto s = registry->interface(Registry::Interface::Shell);
        if (s.name != 0u) {
            const_cast<Integration*>(this)->m_shell = registry->createShell(s.name, s.version, registry);
        }
    }
    return m_shell;
}

EGLDisplay Integration::eglDisplay() const
{
    return m_eglDisplay;
}

QPlatformInputContext *Integration::inputContext() const
{
    return m_inputContext.data();
}

}
}
