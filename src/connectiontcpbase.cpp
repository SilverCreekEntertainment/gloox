/*
  Copyright (c) 2004-2023 by Jakob Schröter <js@camaya.net>
  This file is part of the gloox library. http://camaya.net/gloox

  This software is distributed under a license. The full license
  agreement can be found in the file LICENSE in this distribution.
  This software may not be copied, modified, sold or distributed
  other than expressed in the named license agreement.

  This software is distributed without any warranty.
*/



#include "gloox.h"

#include "connectiontcpbase.h"
#include "dns.h"
#include "logsink.h"
#include "prep.h"
#include "mutexguard.h"
#include "util.h"

#ifdef __MINGW32__
# include <winsock2.h>
# include <ws2tcpip.h>
#endif

#if ( !defined( _WIN32 ) && !defined( _WIN32_WCE ) ) || defined( __SYMBIAN32__ )
# include <arpa/inet.h>
# include <sys/epoll.h>
# include <sys/types.h>
# include <sys/socket.h>
# include <sys/select.h>
# include <netinet/in.h>
# include <unistd.h>
# include <string.h>
# include <errno.h>
# include <netdb.h>
#elif ( defined( _WIN32 ) || defined( _WIN32_WCE ) ) && !defined( __SYMBIAN32__ )
# include <winsock2.h>
# include <ws2tcpip.h>
typedef int socklen_t;
#endif

#include <ctime>

#include <cstdlib>
#include <string>

namespace gloox
{

  ConnectionTCPBase::ConnectionTCPBase( const LogSink& logInstance,
                                        const std::string& server, int port )
    : ConnectionBase( 0 ),
      m_logInstance( logInstance ), m_buf( 0 ), m_socket( -1 ), m_totalBytesIn( 0 ),
      m_totalBytesOut( 0 ), m_bufsize( 8192 ), m_cancel( true )
  {
    init( server, port );
  }

  ConnectionTCPBase::ConnectionTCPBase( ConnectionDataHandler* cdh, const LogSink& logInstance,
                                        const std::string& server, int port )
    : ConnectionBase( cdh ),
      m_logInstance( logInstance ), m_buf( 0 ), m_socket( -1 ), m_totalBytesIn( 0 ),
      m_totalBytesOut( 0 ), m_bufsize( 8192 ), m_cancel( true )
  {
    init( server, port );
  }

  void ConnectionTCPBase::init( const std::string& server, int port )
  {
// FIXME check return value?
    prep::idna( server, m_server );
    m_port = port;
    m_buf = static_cast<char*>( calloc( m_bufsize + 1, sizeof( char ) ) );
  }

  ConnectionTCPBase::~ConnectionTCPBase()
  {
    cleanup();
    free( m_buf );
    m_buf = 0;
  }

  void ConnectionTCPBase::disconnect()
  {
    util::MutexGuard rm( m_recvMutex );
    m_cancel = true;
  }

  bool ConnectionTCPBase::dataAvailable( int timeout )
  {
    if( m_socket < 0 )
      return true; // let recv() catch the closed fd

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
      m_logInstance.err( LogAreaClassConnectionTCPBase, "epoll_create1() failed" );
      return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = m_socket;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, m_socket, &ev) == -1)
    {
      m_logInstance.err( LogAreaClassConnectionTCPBase, "epoll_ctl() failed" );
      close(epoll_fd);
      return false;
    }

    struct epoll_event events[1];
    int nfds = epoll_wait(epoll_fd, events, 1, timeout / 1000); // timeout in milliseconds

    close(epoll_fd);

    return (nfds > 0);
  }

  ConnectionError ConnectionTCPBase::receive()
  {
    if( m_socket < 0 )
      return ConnNotConnected;

    ConnectionError err = ConnNoError;
    while( !m_cancel && ( err = recv( 1000000 ) ) == ConnNoError )
      ;
    return err == ConnNoError ? ConnNotConnected : err;
  }

  bool ConnectionTCPBase::send( const std::string& data )
  {
    m_sendMutex.lock();

    if( data.empty() || ( m_socket < 0 ) )
    {
      m_sendMutex.unlock();
      return false;
    }

    int sent = 0;
    for( size_t num = 0, len = data.length(); sent != -1 && num < len; num += sent )
    {
      sent = static_cast<int>( ::send( m_socket, (data.c_str()+num), static_cast<int>( len - num ), 0 ) );
    }

    m_totalBytesOut += data.length();

    m_sendMutex.unlock();

    if( sent == -1 )
    {
        // send() failed for an unexpected reason
        std::string message = "send() failed. "
#if defined( _WIN32 ) && !defined( __SYMBIAN32__ )
          "WSAGetLastError: " + util::int2string( ::WSAGetLastError() );
#else
          "errno: " + util::int2string( errno ) + ": " + strerror( errno );
#endif
      m_logInstance.err( LogAreaClassConnectionTCPBase, message );

      if( m_handler )
        m_handler->handleDisconnect( this, ConnIoError );
    }

    return sent != -1;
  }

  void ConnectionTCPBase::getStatistics( long int &totalIn, long int &totalOut )
  {
    totalIn = m_totalBytesIn;
    totalOut = m_totalBytesOut;
  }

  void ConnectionTCPBase::cleanup()
  {
    if( !m_sendMutex.trylock() )
      return;

    if( !m_recvMutex.trylock() )
    {
      m_sendMutex.unlock();
      return;
    }

    if( m_socket >= 0 )
    {
      DNS::closeSocket( m_socket, m_logInstance );
      m_socket = -1;
    }

    m_state = StateDisconnected;
    m_cancel = true;
    m_totalBytesIn = 0;
    m_totalBytesOut = 0;

    m_recvMutex.unlock(),
    m_sendMutex.unlock();
  }

  int ConnectionTCPBase::localPort() const
  {
    struct sockaddr local;
    socklen_t len = static_cast<socklen_t>( sizeof( local ) );
    if( getsockname ( m_socket, &local, &len ) < 0 )
      return -1;
    else
      return ntohs( (reinterpret_cast<struct sockaddr_in*>( &local ) )->sin_port );
  }

  const std::string ConnectionTCPBase::localInterface() const
  {
    struct sockaddr_storage local;
    socklen_t len = static_cast<socklen_t>( sizeof( local ) );
    if( getsockname( m_socket, reinterpret_cast<struct sockaddr*>( &local ), &len ) < 0 )
      return EmptyString;
    else
    {
      char buffer[INET6_ADDRSTRLEN];
      int err = getnameinfo( reinterpret_cast<struct sockaddr*>( &local ), len, buffer, sizeof( buffer ),
                             0, 0, NI_NUMERICHOST );
      if( !err )
        return buffer;
      else
        return EmptyString;
    }
  }

}
