/*
    Copyright (c) 2009 Kevin Ottens <ervin@kde.org>

    This library is free software; you can redistribute it and/or modify it
    under the terms of the GNU Library General Public License as published by
    the Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    This library is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
    License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to the
    Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301, USA.
*/

#include "sessionthread_p.h"

#include <QtCore/QDebug>

#include "kimap_debug.h"

#include "imapstreamparser.h"
#include "message_p.h"

using namespace KIMAP;

Q_DECLARE_METATYPE(KTcpSocket::Error)
Q_DECLARE_METATYPE(KSslErrorUiData)
static const int _kimap_socketErrorTypeId = qRegisterMetaType<KTcpSocket::Error>();
static const int _kimap_sslErrorUiData = qRegisterMetaType<KSslErrorUiData>();

SessionThread::SessionThread(const QString &hostName, quint16 port)
    : QObject(), m_hostName(hostName), m_port(port),
      m_socket(Q_NULLPTR), m_stream(nullptr),
      m_encryptedMode(false)
{
    m_socket = new SessionSocket;
    m_stream = new ImapStreamParser(m_socket);
    connect(m_socket, &QIODevice::readyRead,
            this, &SessionThread::readMessage, Qt::QueuedConnection);

    // Delay the call to slotSocketDisconnected so that it finishes disconnecting before we call reconnect()
    connect(m_socket, &KTcpSocket::disconnected,
            this, &SessionThread::slotSocketDisconnected, Qt::QueuedConnection);
    connect(m_socket, &KTcpSocket::connected,
            this, &SessionThread::socketConnected);
    connect(m_socket, SIGNAL(error(KTcpSocket::Error)),
            this, SLOT(slotSocketError(KTcpSocket::Error)));
    connect(m_socket, &QIODevice::bytesWritten,
            this, &SessionThread::socketActivity);
    if (m_socket->metaObject()->indexOfSignal("encryptedBytesWritten(qint64)") > -1) {
        connect(m_socket, &KTcpSocket::encryptedBytesWritten,  // needs kdelibs > 4.8
                this, &SessionThread::socketActivity);
    }
    connect(m_socket, &QIODevice::readyRead,
            this, &SessionThread::socketActivity);

    QMetaObject::invokeMethod(this, "reconnect", Qt::QueuedConnection);
}

SessionThread::~SessionThread()
{
    delete m_stream;
    m_stream = Q_NULLPTR;
    delete m_socket;
    m_socket = Q_NULLPTR;
}

// Called in primary thread
void SessionThread::sendData(const QByteArray &payload)
{
    m_dataQueue.enqueue(payload);
    QMetaObject::invokeMethod(this, "writeDataQueue");
}

// Called in secondary thread
void SessionThread::writeDataQueue()
{
    if (!m_socket) {
        return;
    }

    while (!m_dataQueue.isEmpty()) {
        m_socket->write(m_dataQueue.dequeue());
    }
}

// Called in secondary thread
void SessionThread::readMessage()
{
    if (!m_stream || m_stream->availableDataSize() == 0) {
        return;
    }

    Message message;
    QList<Message::Part> *payload = &message.content;

    try {
        while (!m_stream->atCommandEnd()) {
            if (m_stream->hasString()) {
                QByteArray string = m_stream->readString();
                if (string == "NIL") {
                    *payload << Message::Part(QList<QByteArray>());
                } else {
                    *payload << Message::Part(string);
                }
            } else if (m_stream->hasList()) {
                *payload << Message::Part(m_stream->readParenthesizedList());
            } else if (m_stream->hasResponseCode()) {
                payload = &message.responseCode;
            } else if (m_stream->atResponseCodeEnd()) {
                payload = &message.content;
            } else if (m_stream->hasLiteral()) {
                QByteArray literal;
                while (!m_stream->atLiteralEnd()) {
                    literal += m_stream->readLiteralPart();
                }
                *payload << Message::Part(literal);
            } else {
                // Oops! Something really bad happened, we won't be able to recover
                // so close the socket immediately
                qWarning("Inconsistent state, probably due to some packet loss");
                doCloseSocket();
                return;
            }
        }

        emit responseReceived(message);

    } catch (KIMAP::ImapParserException e) {
        qCWarning(KIMAP_LOG) << "The stream parser raised an exception:" << e.what();
    }

    if (m_stream->availableDataSize() > 1) {
        QMetaObject::invokeMethod(this, "readMessage", Qt::QueuedConnection);
    }

}

// Called in main thread
void SessionThread::closeSocket()
{
    QMetaObject::invokeMethod(this, "doCloseSocket", Qt::QueuedConnection);
}

// Called in secondary thread
void SessionThread::doCloseSocket()
{
    if (!m_socket) {
        return;
    }
    m_encryptedMode = false;
    qCDebug(KIMAP_LOG) << "close";
    m_socket->close();
}

// Called in secondary thread
void SessionThread::reconnect()
{
    if (m_socket == Q_NULLPTR) { // threadQuit already called
        return;
    }
    if (m_socket->state() != SessionSocket::ConnectedState &&
            m_socket->state() != SessionSocket::ConnectingState) {
        if (m_encryptedMode) {
            qCDebug(KIMAP_LOG) << "connectToHostEncrypted" << m_hostName << m_port;
            m_socket->connectToHostEncrypted(m_hostName, m_port);
        } else {
            qCDebug(KIMAP_LOG) << "connectToHost" << m_hostName << m_port;
            m_socket->connectToHost(m_hostName, m_port);
        }
    }
}

// Called in primary thread
void SessionThread::startSsl(KTcpSocket::SslVersion version)
{
    QMetaObject::invokeMethod(this, "doStartSsl", Q_ARG(KTcpSocket::SslVersion, version));
}

// Called in secondary thread (via invokeMethod)
void SessionThread::doStartSsl(KTcpSocket::SslVersion version)
{
    if (!m_socket) {
        return;
    }

    m_socket->setAdvertisedSslVersion(version);
    m_socket->ignoreSslErrors();
    connect(m_socket, &KTcpSocket::encrypted, this, &SessionThread::sslConnected);
    m_socket->startClientEncryption();
}

// Called in secondary thread
void SessionThread::slotSocketDisconnected()
{
    emit socketDisconnected();
}

// Called in secondary thread
void SessionThread::slotSocketError(KTcpSocket::Error error)
{
    if (!m_socket) {
        return;
    }
    Q_UNUSED(error);   // can be used for debugging
    emit socketError(error);
}

// Called in secondary thread
void SessionThread::sslConnected()
{
    if (!m_socket) {
        return;
    }
    KSslCipher cipher = m_socket->sessionCipher();

    if (m_socket->sslErrors().count() > 0 ||
            m_socket->encryptionMode() != KTcpSocket::SslClientMode ||
            cipher.isNull() || cipher.usedBits() == 0) {
        qCDebug(KIMAP_LOG) << "Initial SSL handshake failed. cipher.isNull() is" << cipher.isNull()
                           << ", cipher.usedBits() is" << cipher.usedBits()
                           << ", the socket says:" <<  m_socket->errorString()
                           << "and the list of SSL errors contains"
                           << m_socket->sslErrors().count() << "items.";
        KSslErrorUiData errorData(m_socket);
        emit sslError(errorData);
    } else {
        qCDebug(KIMAP_LOG) << "TLS negotiation done.";
        m_encryptedMode = true;
        emit encryptionNegotiationResult(true, m_socket->negotiatedSslVersion());
    }
}

void SessionThread::sslErrorHandlerResponse(bool response)
{
    QMetaObject::invokeMethod(this, "doSslErrorHandlerResponse", Q_ARG(bool, response));
}

// Called in secondary thread (via invokeMethod)
void SessionThread::doSslErrorHandlerResponse(bool response)
{
    if (!m_socket) {
        return;
    }
    if (response) {
        m_encryptedMode = true;
        emit encryptionNegotiationResult(true, m_socket->negotiatedSslVersion());
    } else {
        m_encryptedMode = false;
        //reconnect in unencrypted mode, so new commands can be issued
        m_socket->disconnectFromHost();
        m_socket->waitForDisconnected();
        m_socket->connectToHost(m_hostName, m_port);
        emit encryptionNegotiationResult(false, KTcpSocket::UnknownSslVersion);
    }
}

#include "moc_sessionthread_p.cpp"
