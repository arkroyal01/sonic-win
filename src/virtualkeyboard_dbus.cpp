/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2017 Martin Flöser <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "virtualkeyboard_dbus.h"
#include <QDBusConnection>

namespace KWin
{

VirtualKeyboardDBus::VirtualKeyboardDBus(QObject *parent)
    : QObject(parent)
{
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/VirtualKeyboard"), this,
                                                 QDBusConnection::ExportAllProperties | QDBusConnection::ExportScriptableContents | // qdbuscpp2xml doesn't support yet properties with NOTIFY
                                                     QDBusConnection::ExportAllSlots);
}

VirtualKeyboardDBus::~VirtualKeyboardDBus() = default;

bool VirtualKeyboardDBus::isActive() const
{
    return false;
}

void VirtualKeyboardDBus::setEnabled(bool enabled)
{
    // No-op (Wayland-only functionality)
}

void VirtualKeyboardDBus::setActive(bool active)
{
    // No-op (Wayland-only functionality)
}

bool VirtualKeyboardDBus::isEnabled() const
{
    return false;
}

bool VirtualKeyboardDBus::isVisible() const
{
    return false;
}

bool VirtualKeyboardDBus::isAvailable() const
{
    return false;
}

bool VirtualKeyboardDBus::activeClientSupportsTextInput() const
{
    return false;
}

bool VirtualKeyboardDBus::willShowOnActive() const
{
    return false;
}

void VirtualKeyboardDBus::forceActivate()
{
    // No-op (Wayland-only functionality)
}
}

#include "moc_virtualkeyboard_dbus.cpp"
