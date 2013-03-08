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
#include <QtCore/QTimer>

#include <KDE/KDebug>

#include "imapstreamparser.h"
#include "message_p.h"
#include "session.h"

using namespace KIMAP;

Q_DECLARE_METATYPE( KTcpSocket::Error )
Q_DECLARE_METATYPE( KSslErrorUiData )
static const int _kimap_socketErrorTypeId = qRegisterMetaType<KTcpSocket::Error>();
static const int _kimap_sslErrorUiData = qRegisterMetaType<KSslErrorUiData>();

SessionThread::SessionThread( const QString &hostName, quint16 port, Session *parent )
  : QThread(), m_hostName( hostName ), m_port( port ),
    m_session( parent ), m_socket( 0 ), m_stream( 0 ), m_mutex( QMutex::Recursive ),
    m_encryptedMode( false ),
    triedSslVersions( 0 ), doSslFallback( false )
{
  // Yeah, sounds weird, but QThread object is linked to the parent
  // thread not to itself, and I'm too lazy to introduce yet another
  // internal QObject
  moveToThread( this );
}

SessionThread::~SessionThread()
{
  // don't call quit() directly, this will deadlock in wait() if exec() hasn't run yet
  QMetaObject::invokeMethod( this, "quit" );
  if ( !wait( 10 * 1000 ) ) {
    kWarning() << "Session thread refuses to die, killing harder...";
    terminate();
    // Make sure to wait until it's done, otherwise it can crash when the pthread callback is called
    wait();
  }
}

void SessionThread::sendData( const QByteArray &payload )
{
  QMutexLocker locker( &m_mutex );

  m_dataQueue.enqueue( payload );
  QTimer::singleShot( 0, this, SLOT(writeDataQueue()) );
}

void SessionThread::writeDataQueue()
{
  QMutexLocker locker( &m_mutex );

  while ( !m_dataQueue.isEmpty() ) {
    m_socket->write( m_dataQueue.dequeue() );
  }
}

void SessionThread::readMessage()
{
  QMutexLocker locker( &m_mutex );

  if ( m_stream->availableDataSize() == 0 ) {
    return;
  }

  Message message;
  QList<Message::Part> *payload = &message.content;

  try {
    while ( !m_stream->atCommandEnd() ) {
      if ( m_stream->hasString() ) {
        QByteArray string = m_stream->readString();
        if ( string == "NIL" ) {
          *payload << Message::Part( QList<QByteArray>() );
        } else {
          *payload << Message::Part( string );
        }
      } else if ( m_stream->hasList() ) {
        *payload << Message::Part( m_stream->readParenthesizedList() );
      } else if ( m_stream->hasResponseCode() ) {
        payload = &message.responseCode;
      } else if ( m_stream->atResponseCodeEnd() ) {
        payload = &message.content;
      } else if ( m_stream->hasLiteral() ) {
        QByteArray literal;
        while ( !m_stream->atLiteralEnd() ) {
          literal += m_stream->readLiteralPart();
        }
        *payload << Message::Part( literal );
      } else {
        // Oops! Something really bad happened, we won't be able to recover
        // so close the socket immediately
        qWarning( "Inconsistent state, probably due to some packet loss" );
        doCloseSocket();
        return;
      }
    }

    emit responseReceived( message );

  } catch ( KIMAP::ImapParserException e ) {
    qWarning() << "The stream parser raised an exception:" << e.what();
  }

  if ( m_stream->availableDataSize() > 1 ) {
    QTimer::singleShot( 0, this, SLOT(readMessage()) );
  }

}

void SessionThread::closeSocket()
{
  QTimer::singleShot( 0, this, SLOT(doCloseSocket()) );
}

void SessionThread::doCloseSocket()
{
  m_encryptedMode = false;
  m_socket->close();
}

void SessionThread::reconnect()
{
  QMutexLocker locker( &m_mutex );

  if ( m_socket->state() != SessionSocket::ConnectedState &&
       m_socket->state() != SessionSocket::ConnectingState ) {
    if ( m_encryptedMode ) {
      m_socket->connectToHostEncrypted( m_hostName, m_port );
    } else {
      m_socket->connectToHost( m_hostName, m_port );
    }
  }
}

void SessionThread::run()
{
  m_socket = new SessionSocket;
  m_stream = new ImapStreamParser( m_socket );
  connect( m_socket, SIGNAL(readyRead()),
           this, SLOT(readMessage()), Qt::QueuedConnection );

  // Delay the call to socketDisconnected so that it finishes disconnecting before we call reconnect()
  connect( m_socket, SIGNAL(disconnected()),
           this, SLOT(socketDisconnected()), Qt::QueuedConnection );
  connect( m_socket, SIGNAL(connected()),
           m_session, SLOT(socketConnected()) );
  connect( m_socket, SIGNAL(error(KTcpSocket::Error)),
           this, SLOT(socketError()) );
  connect( m_socket, SIGNAL(bytesWritten(qint64)),
           m_session, SLOT(socketActivity()) );
  if ( m_socket->metaObject()->indexOfSignal( "encryptedBytesWritten(qint64)" ) > -1 ) {
      connect( m_socket, SIGNAL(encryptedBytesWritten(qint64)), // needs kdelibs > 4.8
               m_session, SLOT(socketActivity()) );
  }
  connect( m_socket, SIGNAL(readyRead()),
           m_session, SLOT(socketActivity()) );

  connect( this, SIGNAL(responseReceived(KIMAP::Message)),
           m_session, SLOT(responseReceived(KIMAP::Message)) );

  QTimer::singleShot( 0, this, SLOT(reconnect()) );
  exec();

  delete m_stream;
  delete m_socket;
}

void SessionThread::startSsl(const KTcpSocket::SslVersion &version)
{
  QMutexLocker locker( &m_mutex );

  if ( version == KTcpSocket::AnySslVersion ) {
    doSslFallback = true;
    if ( m_socket->advertisedSslVersion() == KTcpSocket::UnknownSslVersion ) {
      m_socket->setAdvertisedSslVersion( KTcpSocket::AnySslVersion );
    } else if ( !( triedSslVersions & KTcpSocket::TlsV1 ) ) {
      triedSslVersions |= KTcpSocket::TlsV1;
      m_socket->setAdvertisedSslVersion( KTcpSocket::TlsV1 );
    } else if ( !( triedSslVersions & KTcpSocket::SslV3 ) ) {
      triedSslVersions |= KTcpSocket::SslV3;
      m_socket->setAdvertisedSslVersion( KTcpSocket::SslV3 );
    } else if ( !( triedSslVersions & KTcpSocket::SslV2 ) ) {
      triedSslVersions |= KTcpSocket::SslV2;
      m_socket->setAdvertisedSslVersion( KTcpSocket::SslV2 );
      doSslFallback = false;
    }
  } else {
    m_socket->setAdvertisedSslVersion( version );
  }

  m_socket->ignoreSslErrors();
  connect( m_socket, SIGNAL(encrypted()), this, SLOT(sslConnected()) );
  m_socket->startClientEncryption();
}

void SessionThread::socketDisconnected()
{
  if ( doSslFallback ) {
    reconnect();
  } else {
    QMetaObject::invokeMethod( m_session, "socketDisconnected" );
  }
}

void SessionThread::socketError()
{
  QMutexLocker locker( &m_mutex );
  if ( doSslFallback ) {
    locker.unlock(); // disconnectFromHost() ends up calling reconnect()
    m_socket->disconnectFromHost();
  } else {
    QMetaObject::invokeMethod( m_session, "socketError" );
  }
}

void SessionThread::sslConnected()
{
  QMutexLocker locker( &m_mutex );
  KSslCipher cipher = m_socket->sessionCipher();

  if ( m_socket->sslErrors().count() > 0 ||
       m_socket->encryptionMode() != KTcpSocket::SslClientMode ||
       cipher.isNull() || cipher.usedBits() == 0 ) {
     kDebug() << "Initial SSL handshake failed. cipher.isNull() is" << cipher.isNull()
              << ", cipher.usedBits() is" << cipher.usedBits()
              << ", the socket says:" <<  m_socket->errorString()
              << "and the list of SSL errors contains"
              << m_socket->sslErrors().count() << "items.";
     KSslErrorUiData errorData( m_socket );
     emit sslError( errorData );
  } else {
    doSslFallback = false;
    kDebug() << "TLS negotiation done.";
    m_encryptedMode = true;
    emit encryptionNegotiationResult( true, m_socket->negotiatedSslVersion() );
  }
}

void SessionThread::sslErrorHandlerResponse(bool response)
{
  QMutexLocker locker( &m_mutex );
  if ( response ) {
    m_encryptedMode = true;
    emit encryptionNegotiationResult( true, m_socket->negotiatedSslVersion() );
  } else {
     m_encryptedMode = false;
     //reconnect in unencrypted mode, so new commands can be issued
     m_socket->disconnectFromHost();
     m_socket->waitForDisconnected();
     m_socket->connectToHost( m_hostName, m_port );
     emit encryptionNegotiationResult( false, KTcpSocket::UnknownSslVersion );
  }
}

#include "moc_sessionthread_p.cpp"

