// remote_profile.h
// ---------------------------------------------------------------------------
// RemoteProfile / RemoteProfileNode: the data model behind "Remote
// connections" - one saved host (protocol, address, credentials) plus the
// named-folder tree they're organized under, the same shape
// SessionHistory's SavedSessionNode already uses for saved compare
// sessions in HomeView. Kept as a plain data model with no Qt-widget
// dependencies so it can be shared between RemoteConnectionsDialog (manages
// profiles) and RemotePathBrowserDialog (uses a profile to browse).
// ---------------------------------------------------------------------------
#pragma once

#include <QCoreApplication>
#include <QList>
#include <QString>

namespace openbc::app {

// Every protocol PathSelector's remote browser knows how to reach. SMB and
// NetworkDrive are listed separately because a share name/domain apply only
// to the former (matches the split in the openbc-vfs Rust crate: NetworkVfs
// wraps an already-mounted path, the same thing NetworkDrive means here).
enum class RemoteProtocol { Sftp, Ftp, FtpsExplicit, FtpsImplicit, Smb, NetworkDrive };

inline QList<RemoteProtocol> allRemoteProtocols() {
    return {RemoteProtocol::Sftp,        RemoteProtocol::Ftp,     RemoteProtocol::FtpsExplicit,
            RemoteProtocol::FtpsImplicit, RemoteProtocol::Smb,     RemoteProtocol::NetworkDrive};
}

inline QString protocolLabel(RemoteProtocol protocol) {
    switch (protocol) {
    case RemoteProtocol::Sftp: return QCoreApplication::translate("RemoteProtocol", "SFTP (SSH File Transfer)");
    case RemoteProtocol::Ftp: return QCoreApplication::translate("RemoteProtocol", "FTP");
    case RemoteProtocol::FtpsExplicit: return QCoreApplication::translate("RemoteProtocol", "FTPS (Explicit SSL)");
    case RemoteProtocol::FtpsImplicit: return QCoreApplication::translate("RemoteProtocol", "FTPS (Implicit SSL)");
    case RemoteProtocol::Smb: return QCoreApplication::translate("RemoteProtocol", "SMB / Samba share");
    case RemoteProtocol::NetworkDrive: return QCoreApplication::translate("RemoteProtocol", "Network drive (UNC / mounted path)");
    }
    return {};
}

inline int defaultPortFor(RemoteProtocol protocol) {
    switch (protocol) {
    case RemoteProtocol::Sftp: return 22;
    case RemoteProtocol::Ftp:
    case RemoteProtocol::FtpsExplicit: return 21;
    case RemoteProtocol::FtpsImplicit: return 990;
    case RemoteProtocol::Smb: return 445;
    case RemoteProtocol::NetworkDrive: return 0;
    }
    return 0;
}

inline bool protocolUsesHostPort(RemoteProtocol protocol) { return protocol != RemoteProtocol::NetworkDrive; }
inline bool protocolUsesKeyFile(RemoteProtocol protocol) { return protocol == RemoteProtocol::Sftp; }
inline bool protocolUsesCertFile(RemoteProtocol protocol) {
    return protocol == RemoteProtocol::FtpsExplicit || protocol == RemoteProtocol::FtpsImplicit;
}
inline bool protocolUsesShareName(RemoteProtocol protocol) {
    return protocol == RemoteProtocol::Smb || protocol == RemoteProtocol::NetworkDrive;
}
inline bool protocolUsesPassiveMode(RemoteProtocol protocol) {
    return protocol == RemoteProtocol::Ftp || protocol == RemoteProtocol::FtpsExplicit ||
           protocol == RemoteProtocol::FtpsImplicit;
}
inline bool protocolUsesCredentials(RemoteProtocol protocol) { return protocol != RemoteProtocol::NetworkDrive; }

// One saved host. `password` and `passphrase` only ever hold the *decrypted*
// value while the profile is loaded in memory for editing/connecting; at
// rest RemoteProfileStore keeps them obfuscated (see that file's header
// comment for exactly how safe that is, and how to do better).
struct RemoteProfile {
    QString name;
    RemoteProtocol protocol = RemoteProtocol::Sftp;
    QString host;
    int port = defaultPortFor(RemoteProtocol::Sftp);
    QString username;
    QString password;
    bool savePassword = true;
    QString keyFile;      // SFTP private key path
    QString passphrase;   // SFTP private key passphrase
    QString certFile;     // FTPS client certificate
    QString shareName;    // SMB share, e.g. "Shared" for \\host\Shared
    QString domain;       // SMB domain/workgroup (optional)
    QString mountPath;    // NetworkDrive: local mount point / drive letter / UNC root
    QString remoteRoot = "/";
    bool passiveMode = true;
    QString description;

    bool isValid() const {
        if (name.trimmed().isEmpty()) return false;
        if (protocol == RemoteProtocol::NetworkDrive) return !mountPath.trimmed().isEmpty();
        return !host.trimmed().isEmpty();
    }

    // How the profile is shown once picked as a path, e.g.
    // "sftp://alice@build-server:2222/var/www". Callers that need to route
    // reads through the right openbc-vfs provider should key off `protocol`
    // + this string rather than parsing it back apart.
    QString displayAddress() const {
        if (protocol == RemoteProtocol::NetworkDrive) return mountPath;
        QString scheme;
        switch (protocol) {
        case RemoteProtocol::Sftp: scheme = "sftp"; break;
        case RemoteProtocol::Ftp: scheme = "ftp"; break;
        case RemoteProtocol::FtpsExplicit:
        case RemoteProtocol::FtpsImplicit: scheme = "ftps"; break;
        case RemoteProtocol::Smb: scheme = "smb"; break;
        case RemoteProtocol::NetworkDrive: break;
        }
        QString address = scheme + "://";
        if (!username.isEmpty()) address += username + "@";
        address += host;
        if (port > 0 && port != defaultPortFor(protocol)) address += ":" + QString::number(port);
        if (protocol == RemoteProtocol::Smb && !shareName.isEmpty()) address += "/" + shareName;
        return address;
    }
};

// A named group ("Personal", "Work", ...) that holds profiles and/or child
// groups - mirrors SessionHistory's SavedSessionNode shape used elsewhere
// in HomeView, so the two tree UIs feel identical to a user.
struct RemoteProfileNode {
    QString name;
    QString path;  // unique slash-separated path, e.g. "Personal/Build servers"
    QList<RemoteProfile> profiles;
    QList<RemoteProfileNode> children;
};

}  // namespace openbc::app
