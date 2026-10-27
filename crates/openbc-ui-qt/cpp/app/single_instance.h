#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QtEndian>
#include <functional>

#include "app/cli_options.h"

namespace openbc::app {

class SingleInstance {
public:
    explicit SingleInstance(const LaunchRequest& request)
        : serverName_(serverNameForUser()),
          instanceLock_(QDir::tempPath() + "/" + serverName_ + ".lock") {
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (instanceLock_.tryLock(0)) {
            if (!startServer()) {
                QLocalServer::removeServer(serverName_);
                if (!startServer()) instanceLock_.unlock();
            }
            return;
        }

        forwarded_ = forward(serverName_, request);
    }

    bool isPrimary() const { return primary_; }
    bool wasForwarded() const { return forwarded_; }

    void setLaunchHandler(std::function<void(const LaunchRequest&)> handler) {
        handler_ = std::move(handler);
        const auto pending = std::move(pending_);
        QTimer::singleShot(0, &server_, [this, pending]() {
            for (const LaunchRequest& request : pending) handler_(request);
        });
    }

private:
    static QString serverNameForUser() {
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        return "openbc-" + digest;
    }

    bool startServer() {
        if (!server_.listen(serverName_)) return false;
        primary_ = true;
        QObject::connect(&server_, &QLocalServer::newConnection, &server_, [this]() {
            while (auto* socket = server_.nextPendingConnection()) {
                QObject::connect(socket, &QLocalSocket::readyRead, socket,
                                 [this, socket]() { readRequest(socket); });
                QObject::connect(socket, &QLocalSocket::disconnected, socket, [this, socket]() {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
            }
        });
        return true;
    }

    void readRequest(QLocalSocket* socket) {
        QByteArray& buffer = buffers_[socket];
        buffer += socket->readAll();
        if (buffer.size() < 4) return;
        const auto* bytes = reinterpret_cast<const uchar*>(buffer.constData());
        const quint32 payloadSize = qFromBigEndian<quint32>(bytes);
        if (payloadSize > 1024 * 1024) {
            socket->write("0", 1);
            socket->disconnectFromServer();
            return;
        }
        if (buffer.size() < 4 + payloadSize) return;
        const auto document = QJsonDocument::fromJson(buffer.mid(4, payloadSize));
        if (document.isObject()) {
            const QJsonObject object = document.object();
            LaunchRequest forwarded;
            forwarded.left = object.value("left").toString();
            forwarded.right = object.value("right").toString();
            forwarded.compareType = object.value("compareType").toString();
            forwarded.contextAction = object.value("contextAction").toString();
            forwarded.contextPath = object.value("contextPath").toString();
            socket->write("1", 1);
            socket->flush();
            dispatch(forwarded);
        } else {
            socket->write("0", 1);
        }
        socket->disconnectFromServer();
    }

    static bool forward(const QString& serverName, const LaunchRequest& request) {
        constexpr int timeoutMs = 30000;
        QLocalSocket socket;
        QElapsedTimer timeout;
        timeout.start();
        while (timeout.elapsed() < timeoutMs) {
            socket.connectToServer(serverName);
            if (socket.waitForConnected(100)) break;
            socket.abort();
            QThread::msleep(50);
        }
        if (socket.state() != QLocalSocket::ConnectedState) return false;

        const QJsonObject object{{"left", request.left},
                                 {"right", request.right},
                                 {"compareType", request.compareType},
                                 {"contextAction", request.contextAction},
                                 {"contextPath", request.contextPath}};
        const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QByteArray frame(4, Qt::Uninitialized);
        qToBigEndian<quint32>(static_cast<quint32>(payload.size()),
                              reinterpret_cast<uchar*>(frame.data()));
        frame += payload;
        if (socket.write(frame) != frame.size()) return false;
        while (socket.bytesToWrite() > 0) {
            if (!socket.waitForBytesWritten(1500)) return false;
        }
        const int remainingMs = qMax(1, timeoutMs - static_cast<int>(timeout.elapsed()));
        return socket.waitForReadyRead(remainingMs) && socket.read(1) == QByteArrayLiteral("1");
    }

    void dispatch(const LaunchRequest& request) {
        if (handler_) handler_(request);
        else pending_.push_back(request);
    }

    QLocalServer server_;
    QHash<QLocalSocket*, QByteArray> buffers_;
    std::function<void(const LaunchRequest&)> handler_;
    QVector<LaunchRequest> pending_;
    QString serverName_;
    QLockFile instanceLock_;
    bool primary_ = false;
    bool forwarded_ = false;
};

}  // namespace openbc::app