#include "notificator.h"

#include <QMetaType>
#include <QVariant>
#include <QIcon>
#include <QApplication>
#include <QStyle>
#include <QByteArray>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QTemporaryFile>
#include <QImageWriter>

#ifdef USE_DBUS
#include <QtDBus/QtDBus>
#include <stdint.h>
#endif

#ifdef Q_WS_MAC
#include <ApplicationServices/ApplicationServices.h>
extern bool qt_mac_execute_apple_script(const QString &script, AEDesc *ret);
#endif

// https://wiki.ubuntu.com/NotificationDevelopmentGuidelines recommends at least 128
const int FREEDESKTOP_NOTIFICATION_ICON_SIZE = 128;

Notificator::Notificator(const QString &programName, QSystemTrayIcon *trayicon, QWidget *parent):
    QObject(parent),
    parent(parent),
    programName(programName),
    mode(None),
    trayIcon(trayicon)
#ifdef USE_DBUS
    , interface(0)
#endif
{
    if (trayicon && trayicon->supportsMessages())
    {
        mode = QSystemTray;
    }
#ifdef USE_DBUS
    interface = new QDBusInterface("org.freedesktop.Notifications",
                                   "/org/freedesktop/Notifications", "org.freedesktop.Notifications");
    if (interface->isValid())
    {
        mode = Freedesktop;
    }
#endif
#ifdef Q_WS_MAC
    // Check if Growl is installed (based on Qt's tray icon implementation)
    CFURLRef cfurl;
    OSStatus status = LSGetApplicationForInfo(kLSUnknownType, kLSUnknownCreator, CFSTR("growlTicket"), kLSRolesAll, 0, &cfurl);
    if (status != kLSApplicationNotFoundErr) {
        CFBundleRef bundle = CFBundleCreate(0, cfurl);
        if (CFStringCompare(CFBundleGetIdentifier(bundle), CFSTR("com.Growl.GrowlHelperApp"), kCFCompareCaseInsensitive | kCFCompareBackwards) == kCFCompareEqualTo) {
            if (CFStringHasSuffix(CFURLGetString(cfurl), CFSTR("/Growl.app/")))
                mode = Growl13;
            else
                mode = Growl12;
        }
        CFRelease(cfurl);
        CFRelease(bundle);
    }
#endif
}

Notificator::~Notificator()
{
#ifdef USE_DBUS
    delete interface;
#endif
}

#ifdef USE_DBUS

// Loosely based on http://www.qtcentre.org/archive/index.php/t-25879.html
class FreedesktopImage
{
public:
    FreedesktopImage() {}
    FreedesktopImage(const QImage &img);

    static int metaType();

    // Image to variant that can be marshaled over DBus
    static QVariant toVariant(const QImage &img);

private:
    int width, height, stride;
    bool hasAlpha;
    int channels;
    int bitsPerSample;
    QByteArray image;

    friend QDBusArgument &operator<<(QDBusArgument &a, const FreedesktopImage &i);
    friend const QDBusArgument &operator>>(const QDBusArgument &a, FreedesktopImage &i);
};

Q_DECLARE_METATYPE(FreedesktopImage);

// Image configuration settings
const int CHANNELS = 4;
const int BYTES_PER_PIXEL = 4;
const int BITS_PER_SAMPLE = 8;

FreedesktopImage::FreedesktopImage(const QImage &img):
    width(img.width()),
    height(img.height()),
    stride(img.width() * BYTES_PER_PIXEL),
    hasAlpha(true),
    channels(CHANNELS),
    bitsPerSample(BITS_PER_SAMPLE)
{
    // Convert 00xAARRGGBB to RGBA bytewise (endian-independent) format
    QImage tmp = img.convertToFormat(QImage::Format_ARGB32);
    const uint32_t *data = reinterpret_cast<const uint32_t*>(tmp.constBits());

    unsigned int num_pixels = width * height;
    image.resize(num_pixels * BYTES_PER_PIXEL);

    for (unsigned int ptr = 0; ptr < num_pixels; ++ptr)
    {
        image[ptr * BYTES_PER_PIXEL + 0] = data[ptr] >> 16; // R
        image[ptr * BYTES_PER_PIXEL + 1] = data[ptr] >> 8; // G
        image[ptr * BYTES_PER_PIXEL + 2] = data[ptr];   // B
        image[ptr * BYTES_PER_PIXEL + 3] = data[ptr] >> 24; // A
    }
}

QDBusArgument &operator<<(QDBusArgument &a, const FreedesktopImage &i)
{
    a.beginStructure();
    a << i.width << i.height << i.stride << i.hasAlpha << i.bitsPerSample << i.channels << i.image;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, FreedesktopImage &i)
{
    a.beginStructure();
    a >> i.width >> i.height >> i.stride >> i.hasAlpha >> i.bitsPerSample >> i.channels >> i.image;
    a.endStructure();
    return a;
}

int FreedesktopImage::metaType()
{
    return qDBusRegisterMetaType<FreedesktopImage>();
}

QVariant FreedesktopImage::toVariant(const QImage &img)
{
    FreedesktopImage fimg(img);
    return QVariant(FreedesktopImage::metaType(), &fimg);
}

void Notificator::notifyDBus(Class cls, const QString &title, const QString &text, const QIcon &icon, int millisTimeout)
{
    Q_UNUSED(cls);
    // Arguments for DBus call:
    QList<QVariant> args;

    // Program Name:
    args.append(programName);

    // Unique ID of this notification type:
    args.append(0U);

    // Application Icon, empty string
    args.append(QString());

    // Summary
    args.append(title);

    // Body
    args.append(text);

    // Actions (none, actions are deprecated)
    QStringList actions;
    args.append(actions);

    // Hints
    QVariantMap hints;

    // If no icon specified, set icon based on class
    QIcon tmpicon;
    if (icon.isNull())
    {
        QStyle::StandardPixmap sicon = QStyle::SP_MessageBoxQuestion;
        switch (cls)
        {
        case Information: sicon = QStyle::SP_MessageBoxInformation; break;
        case Warning: sicon = QStyle::SP_MessageBoxWarning; break;
        case Critical: sicon = QStyle::SP_MessageBoxCritical; break;
        default: break;
        }
        tmpicon = QApplication::style()->standardIcon(sicon);
    }
    else
    {
        tmpicon = icon;
    }
    hints["icon_data"] = FreedesktopImage::toVariant(tmpicon.pixmap(FREEDESKTOP_NOTIFICATION_ICON_SIZE).toImage());
    args.append(hints);

    // Timeout (in msec)
    args.append(millisTimeout);

    // "Fire and forget"
    interface->callWithArgumentList(QDBus::NoBlock, "Notify", args);
}
#endif

void Notificator::notifySystray(Class cls, const QString &title, const QString &text, const QIcon &icon, int millisTimeout)
{
    Q_UNUSED(icon);
    QSystemTrayIcon::MessageIcon sicon = QSystemTrayIcon::NoIcon;
    switch (cls) // Set icon based on class
    {
    case Information: sicon = QSystemTrayIcon::Information; break;
    case Warning: sicon = QSystemTrayIcon::Warning; break;
    case Critical: sicon = QSystemTrayIcon::Critical; break;
    }
    trayIcon->showMessage(title, text, sicon, millisTimeout);
}

// Based on Qt's tray icon implementation
#ifdef Q_WS_MAC
void Notificator::notifyGrowl(Class cls, const QString &title, const QString &text, const QIcon &icon)
{
    const QString script(
        "tell application \"%5\"\n"
        "  set the allNotificationsList to {\"Notification\"}\n" // -- Make a list of all the notification types (all)
        "  set the enabledNotificationsList to {\"Notification\"}\n" // -- Make a list of the notifications (enabled)
        "  register as application \"%1\" all notifications allNotificationsList default notifications enabledNotificationsList\n" // -- Register our script with Growl
        "  notify with name \"Notification\" title \"%2\" description \"%3\" application name \"%1\"%4\n" // -- Send a Notification
        "end tell"
    );

    QString notificationApp(QApplication::applicationName());
    if (notificationApp.isEmpty())
        notificationApp = "Application";

    QPixmap notificationIconPixmap;
    if (icon.isNull()) { // If no icon specified, set icon based on class
        QStyle::StandardPixmap sicon = QStyle::SP_MessageBoxQuestion;
        switch (cls)
        {
        case Information: sicon = QStyle::SP_MessageBoxInformation; break;
        case Warning: sicon = QStyle::SP_MessageBoxWarning; break;
        case Critical: sicon = QStyle::SP_MessageBoxCritical; break;
        }
        notificationIconPixmap = QApplication::style()->standardPixmap(sicon);
    }
    else {
        QSize size = icon.actualSize(QSize(48, 48));
        notificationIconPixmap = icon.pixmap(size);
    }

    QString notificationIcon;
    QTemporaryFile notificationIconFile;
    if (!notificationIconPixmap.isNull() && notificationIconFile.open()) {
        QImageWriter writer(&notificationIconFile, "PNG");
        if (writer.write(notificationIconPixmap.toImage()))
            notificationIcon = QString(" image from location \"file://%1\"").arg(notificationIconFile.fileName());
    }

    QString quotedTitle(title), quotedText(text);
    quotedTitle.replace("\\", "\\\\").replace("\"", "\\");
    quotedText.replace("\\", "\\\\").replace("\"", "\\");
    QString growlApp(this->mode == Notificator::Growl13 ? "Growl" : "GrowlHelperApp");
    qt_mac_execute_apple_script(script.arg(notificationApp, quotedTitle, quotedText, notificationIcon, growlApp), 0);
}
#endif

void Notificator::notify(Class cls, const QString &title, const QString &text, const QIcon &icon, int millisTimeout)
{
    switch (mode)
    {
#ifdef USE_DBUS
    case Freedesktop:
        notifyDBus(cls, title, text, icon, millisTimeout);
        break;
#endif
    case QSystemTray:
        notifySystray(cls, title, text, icon, millisTimeout);
        break;
#ifdef Q_WS_MAC
    case Growl12:
    case Growl13:
        notifyGrowl(cls, title, text, icon);
        break;
#endif
    default:
        if (cls == Critical)
        {
            // Fall back to old fashioned popup dialog if critical and no other notification available
            QMessageBox::critical(parent, title, text, QMessageBox::Ok, QMessageBox::Ok);
        }
        break;
    }
}
