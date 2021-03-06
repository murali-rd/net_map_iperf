/*---------------------------------------------------------------
 * Copyright (c) 1999,2000,2001,2002,2003
 * The Board of Trustees of the University of Illinois
 * All Rights Reserved.
 *---------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software (Iperf) and associated
 * documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 *
 * Redistributions of source code must retain the above
 * copyright notice, this list of conditions and
 * the following disclaimers.
 *
 *
 * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimers in the documentation and/or other materials
 * provided with the distribution.
 *
 *
 * Neither the names of the University of Illinois, NCSA,
 * nor the names of its contributors may be used to endorse
 * or promote products derived from this Software without
 * specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ________________________________________________________________
 * National Laboratory for Applied Network Research
 * National Center for Supercomputing Applications
 * University of Illinois at Urbana-Champaign
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________
 *
 * Listener.cpp
 * by Mark Gates <mgates@nlanr.net>
 * &  Ajay Tirumala <tirumala@ncsa.uiuc.edu>
 * -------------------------------------------------------------------
 * Listener sets up a socket listening on the server host. For each
 * connected socket that accept() returns, this creates a Server
 * socket and spawns a thread for it.
 *
 * Changes to the latest version. Listener will run as a daemon
 * Multicast Server is now Multi-threaded
 * -------------------------------------------------------------------
 * headers
 * uses
 *   <stdlib.h>
 *   <stdio.h>
 *   <string.h>
 *   <errno.h>
 *
 *   <sys/types.h>
 *   <unistd.h>
 *
 *   <netdb.h>
 *   <netinet/in.h>
 *   <sys/socket.h>
 * ------------------------------------------------------------------- */


#define HEADERS()

#include "headers.h"
#include "Listener.hpp"
#include "SocketAddr.h"
#include "PerfSocket.hpp"
#include "List.h"
#include "util.h"
#include "version.h"
#include "Locale.h"
#include "lwip_adap.h"

/* -------------------------------------------------------------------
 * Stores local hostname and socket info.
 * ------------------------------------------------------------------- */

Listener::Listener( thread_Settings *inSettings ) {

    mClients = inSettings->mThreads;
    mBuf = NULL;
    /*
     * These thread settings are stored in three places
     *
     * 1) Listener thread
     * 2) Reporter Thread (per the ReportSettings())
     * 3) Server thread
     */
    mSettings = inSettings;

    // initialize buffer for packets
    mBuf = new char[((mSettings->mBufLen > SIZEOF_MAXHDRMSG) ? mSettings->mBufLen : SIZEOF_MAXHDRMSG)];
    FAIL_errno( mBuf == NULL, "No memory for buffer\n", mSettings );
    /*
     *  Perform listener threads length checks
     */
    if (isUDP(mSettings)) {
	if (!isCompat(inSettings) && (mSettings->mBufLen < SIZEOF_UDPHDRMSG)) {
	    fprintf(stderr, warn_len_too_small_peer_exchange, "Listener",  mSettings->mBufLen, SIZEOF_UDPHDRMSG);
	}
	if (mSettings->mBufLen < (int) sizeof( UDP_datagram ) ) {
	    mSettings->mBufLen = sizeof( UDP_datagram );
	    fprintf( stderr, warn_buffer_too_small, "Listener", mSettings->mBufLen );
	}
    } else {
	if (!isCompat(mSettings) && (mSettings->mBufLen < SIZEOF_TCPHDRMSG)) {
	    fprintf(stderr, warn_len_too_small_peer_exchange, "Listener", mSettings->mBufLen, SIZEOF_TCPHDRMSG);
	}
    }
    // Now hang the listening on the socket
    Listen( );

    ReportSettings( inSettings );
} // end Listener

/* -------------------------------------------------------------------
 * Delete memory (buffer).
 * ------------------------------------------------------------------- */
Listener::~Listener() {
    if ( mSettings->mSock != INVALID_SOCKET ) {
        int rc = close( mSettings->mSock );
        WARN_errno( rc == SOCKET_ERROR, "close" );
        mSettings->mSock = INVALID_SOCKET;
    }
    DELETE_ARRAY( mBuf );
} // end ~Listener

/* -------------------------------------------------------------------
 * Listens for connections and starts Servers to handle data.
 * For TCP, each accepted connection spawns a Server thread.
 * For UDP, handle all data in this thread for Win32 Only, otherwise
 *          spawn a new Server thread.
 * ------------------------------------------------------------------- */
void Listener::Run( void ) {
#if 0 // ifdef WIN32 removed to allow Windows to use multi-threaded UDP server
    if ( isUDP( mSettings ) && !isSingleUDP( mSettings ) ) {
        UDPSingleServer();
    } else
#else
#ifdef sun
    if ( ( isUDP( mSettings ) &&
           isMulticast( mSettings ) &&
           !isSingleUDP( mSettings ) ) ||
         isSingleUDP( mSettings ) ) {
        UDPSingleServer();
    } else
#else
    if ( isSingleUDP( mSettings ) ) {
        UDPSingleServer();
    } else
#endif
#endif
    {
        bool client = false, UDP = isUDP( mSettings ), mCount = (mSettings->mThreads != 0);
        thread_Settings *tempSettings = NULL;
        Iperf_ListEntry *exist, *listtemp;
        client_hdr* hdr = ( UDP ? (client_hdr*) (((UDP_datagram*)mBuf) + 1) :
                                  (client_hdr*) mBuf);

        if ( mSettings->mHost != NULL ) {
            client = true;
            SockAddr_remoteAddr( mSettings );
        }
        Settings_Copy( mSettings, &server );
        server->mThreadMode = kMode_Server;


        // Accept each packet,
        // If there is no existing client, then start
        // a new thread to service the new client
        // The listener runs in a single thread
        // Thread per client model is followed
        do {
            // Get a new socket
            Accept( server );
            if ( server->mSock == INVALID_SOCKET ) {
                break;
            }
            if ( sInterupted != 0 ) {
		// In the case of -r, ignore the clients alarm
		if (
#if HAVE_DECL_SIGALRM
sInterupted == SIGALRM
#else
0
#endif
		    ) {
		    sInterupted = 0;
		} else {
		    close( server->mSock );
		    break;
		}
            }
            // Reset Single Client Stuff
            if ( isSingleClient( mSettings ) && clients == NULL ) {
                mSettings->peer = server->peer;
                mClients--;
                client = true;
                // Once all the server threads exit then quit
                // Must keep going in case this client sends
                // more streams
                if ( mClients == 0 ) {
                    thread_release_nonterm( 0 );
                    mClients = 1;
                }
            }
            // Verify that it is allowed
            if ( client ) {
                if ( !SockAddr_Hostare_Equal( (sockaddr*) &mSettings->peer,
                                              (sockaddr*) &server->peer ) ) {
                    // Not allowed try again
                    close( server->mSock );
                    if ( isUDP( mSettings ) ) {
                        mSettings->mSock = -1;
                        Listen();
                    }
                    continue;
                }
            }
            // Check for exchange of test information and also determine v2.0.5 vs 2.0.10+
            if ( !isCompat( mSettings ) && !isMulticast( mSettings ) ) {
                if (ReadClientHeader(hdr) < 0) {
		    close( server->mSock );
		    continue;
		}
		// The following will set the tempSettings to NULL if
		// there is no need for the Listener to start a client
                Settings_GenerateClientSettings( server, &tempSettings, hdr );
            } else {
	        tempSettings = NULL;
	    }
            if ( tempSettings != NULL ) {
                client_init( tempSettings );
                if ( tempSettings->mMode == kTest_DualTest ) {
#ifdef HAVE_THREAD
                    server->runNow =  tempSettings;
#else
                    server->runNext = tempSettings;
#endif
                } else {
                    server->runNext =  tempSettings;
                }
            }

            // Create an entry for the connection list
            listtemp = new Iperf_ListEntry;
            memcpy(listtemp, &server->peer, sizeof(iperf_sockaddr));
            listtemp->next = NULL;

            // See if we need to do summing
            Mutex_Lock( &clients_mutex );
            exist = Iperf_hostpresent( &server->peer, clients);

            if ( exist != NULL ) {
                // Copy group ID
                listtemp->holder = exist->holder;
                server->multihdr = exist->holder;
            } else {
                Mutex_Lock( &groupCond );
                groupID--;
                listtemp->holder = InitMulti( server, groupID );
                server->multihdr = listtemp->holder;
                Mutex_Unlock( &groupCond );
            }

            // Store entry in connection list
            Iperf_pushback( listtemp, &clients );
            Mutex_Unlock( &clients_mutex );

            // Start the server
#if defined(WIN32) && defined(HAVE_THREAD)
            if ( UDP ) {
                // WIN32 does bad UDP handling so run single threaded
                if ( server->runNow != NULL ) {
                    thread_start( server->runNow );
                }
                server_spawn( server );
                if ( server->runNext != NULL ) {
                    thread_start( server->runNext );
                }
            } else
#endif
            thread_start( server );

            // create a new socket
            if ( UDP ) {
                mSettings->mSock = -1;
                Listen( );
            }

            // Prep for next connection
            if ( !isSingleClient( mSettings ) ) {
                mClients--;
            }
            Settings_Copy( mSettings, &server );
            server->mThreadMode = kMode_Server;
        } while ( !sInterupted && (!mCount || ( mCount && mClients > 0 )) );

        Settings_Destroy( server );
    }
} // end Run

/* -------------------------------------------------------------------
 * Setup a socket listening on a port.
 * For TCP, this calls bind() and listen().
 * For UDP, this just calls bind().
 * If inLocalhost is not null, bind to that address rather than the
 * wildcard server address, specifying what incoming interface to
 * accept connections on.
 * ------------------------------------------------------------------- */
void Listener::Listen( ) {
    int rc;

    SockAddr_localAddr( mSettings );

    // create an internet TCP socket
    int type = (isUDP( mSettings )  ?  SOCK_DGRAM  :  SOCK_STREAM);
    int domain = (SockAddr_isIPv6( &mSettings->local ) ?
#ifdef HAVE_IPV6
                  AF_INET6
#else
                  AF_INET
#endif
                  : AF_INET);

#ifdef WIN32
    if ( SockAddr_isMulticast( &mSettings->local ) ) {
        // Multicast on Win32 requires special handling
        mSettings->mSock = WSASocket( domain, type, 0, 0, 0, WSA_FLAG_MULTIPOINT_C_LEAF | WSA_FLAG_MULTIPOINT_D_LEAF );
        WARN_errno( mSettings->mSock == INVALID_SOCKET, "socket" );

    } else
#endif
    {
        mSettings->mSock = socket( domain, type, 0 );
        WARN_errno( mSettings->mSock == INVALID_SOCKET, "socket" );
    }

    SetSocketOptions( mSettings );

    // reuse the address, so we can run if a former server was killed off
    int boolean = 1;
    Socklen_t len = sizeof(boolean);
    setsockopt( mSettings->mSock, SOL_SOCKET, SO_REUSEADDR, (char*) &boolean, len );

    // bind socket to server address
#ifdef WIN32
    if ( SockAddr_isMulticast( &mSettings->local ) ) {
        // Multicast on Win32 requires special handling
        rc = WSAJoinLeaf( mSettings->mSock, (sockaddr*) &mSettings->local, mSettings->size_local,0,0,0,0,JL_BOTH);
        WARN_errno( rc == SOCKET_ERROR, "WSAJoinLeaf (aka bind)" );
    } else
#endif
    {
        rc = bind( mSettings->mSock, (sockaddr*) &mSettings->local, mSettings->size_local );
        FAIL_errno( rc == SOCKET_ERROR, "bind", mSettings );
    }
    // listen for connections (TCP only).
    // default backlog traditionally 5
    if ( !isUDP( mSettings ) ) {
        rc = listen( mSettings->mSock, 5 );
        WARN_errno( rc == SOCKET_ERROR, "listen" );
    }

#ifndef WIN32
    // if multicast, join the group
    if ( SockAddr_isMulticast( &mSettings->local ) ) {
        McastJoin( );
    }
#endif
} // end Listen

/* -------------------------------------------------------------------
 * Joins the multicast group, with the default interface.
 * ------------------------------------------------------------------- */

void Listener::McastJoin( ) {
#ifdef HAVE_MULTICAST
    if ( !SockAddr_isIPv6( &mSettings->local ) ) {
        struct ip_mreq mreq;

        memcpy( &mreq.imr_multiaddr, SockAddr_get_in_addr( &mSettings->local ),
                sizeof(mreq.imr_multiaddr));

        mreq.imr_interface.s_addr = htonl( INADDR_ANY );

        int rc = setsockopt( mSettings->mSock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                             (char*) &mreq, sizeof(mreq));
        WARN_errno( rc == SOCKET_ERROR, "multicast join" );
    }
#ifdef HAVE_IPV6_MULTICAST
      else {
        struct ipv6_mreq mreq;

        memcpy( &mreq.ipv6mr_multiaddr, SockAddr_get_in6_addr( &mSettings->local ),
                sizeof(mreq.ipv6mr_multiaddr));

        mreq.ipv6mr_interface = 0;

        int rc = setsockopt( mSettings->mSock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                             (char*) &mreq, sizeof(mreq));
        WARN_errno( rc == SOCKET_ERROR, "multicast join" );
    }
#endif
#endif
}
// end McastJoin

/* -------------------------------------------------------------------
 * Sets the Multicast TTL for outgoing packets.
 * ------------------------------------------------------------------- */

void Listener::McastSetTTL( int val ) {
#ifdef HAVE_MULTICAST
    if ( !SockAddr_isIPv6( &mSettings->local ) ) {
        int rc = setsockopt( mSettings->mSock, IPPROTO_IP, IP_MULTICAST_TTL,
                             (char*) &val, sizeof(val));
        WARN_errno( rc == SOCKET_ERROR, "multicast ttl" );
    }
#ifdef HAVE_IPV6_MULTICAST
      else {
        int rc = setsockopt( mSettings->mSock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                             (char*) &val, sizeof(val));
        WARN_errno( rc == SOCKET_ERROR, "multicast ttl" );
    }
#endif
#endif
}
// end McastSetTTL

/* -------------------------------------------------------------------
 * After Listen() has setup mSock, this will block
 * until a new connection arrives or until the -t value occurs
 * ------------------------------------------------------------------- */

void Listener::Accept( thread_Settings *server ) {

    server->size_peer = sizeof(iperf_sockaddr);
    // Handles interupted accepts. Returns the newly connected socket.
    server->mSock = INVALID_SOCKET;

    bool mMode_Time = isServerModeTime( mSettings ) && !isDaemon( mSettings );
    // setup termination variables
    if ( mMode_Time ) {
	mEndTime.setnow();
	mEndTime.add( mSettings->mAmount / 100.0 );
	if (!setsock_blocking(mSettings->mSock, 0)) {
	    WARN(1, "Failed setting socket to non-blocking mode");
	}
    }

    while ( server->mSock == INVALID_SOCKET) {
	if (mMode_Time) {
	    struct timeval t1;
	    gettimeofday( &t1, NULL );
	    if (mEndTime.before( t1)) {
		break;
	    }
	    struct timeval timeout;
	    timeout.tv_sec = mSettings->mAmount / 100;
	    timeout.tv_usec = (mSettings->mAmount % 100) * 10000;
	    fd_set set;
	    FD_ZERO(&set);
	    FD_SET(mSettings->mSock, &set);
	    if (select( mSettings->mSock + 1, &set, NULL, NULL, &timeout) <= 0) {
		break;
	    }
	}
	if ( isUDP( server ) ) {
	    /* ------------------------------------------------------------------------
	     * Do the equivalent of an accept() call for UDP sockets. This waits
	     * on a listening UDP socket until we get a datagram.
	     * ------------------------------------------------------------------- ----*/
	    int rc;
	    Iperf_ListEntry *exist;
	    int32_t datagramID;
	    server->mSock = INVALID_SOCKET;

	    rc = recvfrom( mSettings->mSock, mBuf, mSettings->mBufLen, 0,
			   (struct sockaddr*) &server->peer, &server->size_peer );
	    FAIL_errno( rc == SOCKET_ERROR, "recvfrom", mSettings );
	    Mutex_Lock( &clients_mutex );

	    // Handle connection for UDP sockets.
	    exist = Iperf_present( &server->peer, clients);
	    datagramID = ntohl( ((UDP_datagram*) mBuf)->id );
	    if ( exist == NULL && datagramID >= 0 ) {
		server->mSock = mSettings->mSock;
		int rc = connect( server->mSock, (struct sockaddr*) &server->peer,
				  server->size_peer );
		FAIL_errno( rc == SOCKET_ERROR, "connect UDP", mSettings );
	    } else {
		server->mSock = INVALID_SOCKET;
	    }
	    Mutex_Unlock( &clients_mutex );
	} else {
	    // accept a TCP  connection
	    server->mSock = accept( mSettings->mSock,  (sockaddr*) &server->peer, &server->size_peer );
	    if ( server->mSock == INVALID_SOCKET &&
#if WIN32
		 WSAGetLastError() == WSAEINTR
#else
		 errno == EINTR
#endif
		) {
		break;
	    }
	}
    }
    if (server->mSock != INVALID_SOCKET) {
	if (!setsock_blocking(server->mSock, 1)) {
	    WARN(1, "Failed setting socket to blocking mode");
	}
    }
    server->size_local = sizeof(iperf_sockaddr);
    getsockname( server->mSock, (sockaddr*) &server->local, &server->size_local );
} // end Accept

void Listener::UDPSingleServer( ) {

    bool client = false, UDP = isUDP( mSettings ), mCount = (mSettings->mThreads != 0);
    thread_Settings *tempSettings = NULL;
    Iperf_ListEntry *exist, *listtemp;
    int rc;
    int32_t datagramID;
    client_hdr* hdr = ( UDP ? (client_hdr*) (((UDP_datagram*)mBuf) + 1) :
                              (client_hdr*) mBuf);
    ReportStruct *reportstruct = new ReportStruct;
    bool mMode_Time = isServerModeTime( mSettings ) && !isDaemon( mSettings );
    // setup termination variables
    if ( mMode_Time ) {
	mEndTime.setnow();
	mEndTime.add( mSettings->mAmount / 100.0 );
    }

    if ( mSettings->mHost != NULL ) {
        client = true;
        SockAddr_remoteAddr( mSettings );
    }
    Settings_Copy( mSettings, &server );
    server->mThreadMode = kMode_Server;


    // Accept each packet,
    // If there is no existing client, then start
    // a new report to service the new client
    // The listener runs in a single thread
    Mutex_Lock( &clients_mutex );
    do {
        // Get next packet
        while ( sInterupted == 0) {
            server->size_peer = sizeof( iperf_sockaddr );

	    if (mMode_Time) {
		struct timeval t1;
		gettimeofday( &t1, NULL );
		if (mEndTime.before( t1)) {
		    sInterupted = 1;
		    break;
		}
		struct timeval timeout;
		timeout.tv_sec = mSettings->mAmount / 100;
		timeout.tv_usec = (mSettings->mAmount % 100) * 10000;
		fd_set set;
		FD_ZERO(&set);
		FD_SET(mSettings->mSock, &set);
		if (select( mSettings->mSock + 1, &set, NULL, NULL, &timeout) <= 0) {
		    sInterupted = 1;
		    break;
		}
	    }

            rc = recvfrom( mSettings->mSock, mBuf, mSettings->mBufLen, 0,
                           (struct sockaddr*) &server->peer, &server->size_peer );
            WARN_errno( rc == SOCKET_ERROR, "recvfrom" );
            if ( rc == SOCKET_ERROR ) {
                return;
            }


            // Handle connection for UDP sockets.
            exist = Iperf_present( &server->peer, clients);
            datagramID = ntohl( ((UDP_datagram*) mBuf)->id );
            if ( datagramID >= 0 ) {
                if ( exist != NULL ) {
                    // read the datagram ID and sentTime out of the buffer
                    reportstruct->packetID = datagramID;
                    reportstruct->sentTime.tv_sec = ntohl( ((UDP_datagram*) mBuf)->tv_sec  );
                    reportstruct->sentTime.tv_usec = ntohl( ((UDP_datagram*) mBuf)->tv_usec );

                    reportstruct->packetLen = rc;
                    gettimeofday( &(reportstruct->packetTime), NULL );

                    ReportPacket( exist->server->reporthdr, reportstruct );
                } else {
                    Mutex_Lock( &groupCond );
                    groupID--;
                    server->mSock = -groupID;
                    Mutex_Unlock( &groupCond );
                    server->size_local = sizeof(iperf_sockaddr);
                    getsockname( mSettings->mSock, (sockaddr*) &server->local, \
                                 &server->size_local );
                    break;
                }
            } else {
                if ( exist != NULL ) {
                    // read the datagram ID and sentTime out of the buffer
                    reportstruct->packetID = -datagramID;
                    reportstruct->sentTime.tv_sec = ntohl( ((UDP_datagram*) mBuf)->tv_sec  );
                    reportstruct->sentTime.tv_usec = ntohl( ((UDP_datagram*) mBuf)->tv_usec );

                    reportstruct->packetLen = rc;
                    gettimeofday( &(reportstruct->packetTime), NULL );

                    ReportPacket( exist->server->reporthdr, reportstruct );
                    // stop timing
                    gettimeofday( &(reportstruct->packetTime), NULL );
                    CloseReport( exist->server->reporthdr, reportstruct );

                    if (rc > (int) (sizeof(UDP_datagram) + sizeof(server_hdr))) {
                        UDP_datagram *UDP_Hdr;
                        server_hdr *hdr;

                        UDP_Hdr = (UDP_datagram*) mBuf;
                        Transfer_Info *stats = GetReport( exist->server->reporthdr );
                        hdr = (server_hdr*) (UDP_Hdr+1);

                        hdr->base.flags        = htonl( HEADER_VERSION1 );
                        hdr->base.total_len1   = htonl( (long) (stats->TotalLen >> 32) );
                        hdr->base.total_len2   = htonl( (long) (stats->TotalLen & 0xFFFFFFFF) );
                        hdr->base.stop_sec     = htonl( (long) stats->endTime );
                        hdr->base.stop_usec    = htonl( (long)((stats->endTime - (long)stats->endTime) \
                                                          * rMillion));
                        hdr->base.error_cnt    = htonl( stats->cntError );
                        hdr->base.outorder_cnt = htonl( stats->cntOutofOrder );
                        hdr->base.datagrams    = htonl( stats->cntDatagrams );
                        hdr->base.jitter1      = htonl( (long) stats->jitter );
                        hdr->base.jitter2      = htonl( (long) ((stats->jitter - (long)stats->jitter) \
                                                           * rMillion) );
                    }
                    EndReport( exist->server->reporthdr );
                    exist->server->reporthdr = NULL;
                    Iperf_delete( &(exist->server->peer), &clients );
                } else if (rc > (int) (sizeof(UDP_datagram) + sizeof(server_hdr))) {
                    UDP_datagram *UDP_Hdr;
                    server_hdr *hdr;

                    UDP_Hdr = (UDP_datagram*) mBuf;
                    hdr = (server_hdr*) (UDP_Hdr+1);
                    hdr->base.flags = htonl( 0 );
                }
                sendto( mSettings->mSock, mBuf, mSettings->mBufLen, 0, \
                        (struct sockaddr*) &server->peer, server->size_peer);
            }
        }
        if ( server->mSock == INVALID_SOCKET ) {
            break;
        }
        if ( sInterupted != 0 ) {
            close( server->mSock );
            break;
        }
        // Reset Single Client Stuff
        if ( isSingleClient( mSettings ) && clients == NULL ) {
            mSettings->peer = server->peer;
            mClients--;
            client = true;
            // Once all the server threads exit then quit
            // Must keep going in case this client sends
            // more streams
            if ( mClients == 0 ) {
                thread_release_nonterm( 0 );
                mClients = 1;
            }
        }
        // Verify that it is allowed
        if ( client ) {
            if ( !SockAddr_Hostare_Equal( (sockaddr*) &mSettings->peer, \
                                          (sockaddr*) &server->peer ) ) {
                // Not allowed try again
                connect( mSettings->mSock,
                         (sockaddr*) &server->peer,
                         server->size_peer );
                close( mSettings->mSock );
                mSettings->mSock = -1;
                Listen( );
                continue;
            }
        }

        // Create an entry for the connection list
        listtemp = new Iperf_ListEntry;
        memcpy(listtemp, &server->peer, sizeof(iperf_sockaddr));
        listtemp->server = server;
        listtemp->next = NULL;

        // See if we need to do summing
        exist = Iperf_hostpresent( &server->peer, clients);

        if ( exist != NULL ) {
            // Copy group ID
            listtemp->holder = exist->holder;
            server->multihdr = exist->holder;
        } else {
            Mutex_Lock( &groupCond );
            groupID--;
            listtemp->holder = InitMulti( server, groupID );
            server->multihdr = listtemp->holder;
            Mutex_Unlock( &groupCond );
        }

        // Store entry in connection list
        Iperf_pushback( listtemp, &clients );

        if ( !isCompat( mSettings ) && !isMulticast( mSettings ) ) {
            Settings_GenerateClientSettings( server, &tempSettings,
                                              hdr );
        } else {
	    tempSettings = NULL;
	}

        if ( tempSettings != NULL ) {
            client_init( tempSettings );
            if ( tempSettings->mMode == kTest_DualTest ) {
#ifdef HAVE_THREAD
                thread_start( tempSettings );
#else
                server->runNext = tempSettings;
#endif
            } else {
                server->runNext =  tempSettings;
            }
        }
        server->reporthdr = InitReport( server );

        // Prep for next connection
        if ( !isSingleClient( mSettings ) ) {
            mClients--;
        }
        Settings_Copy( mSettings, &server );
        server->mThreadMode = kMode_Server;
    } while ( !sInterupted && (!mCount || ( mCount && mClients > 0 )) );
    Mutex_Unlock( &clients_mutex );

    Settings_Destroy( server );
}

int Listener::ReadClientHeader(client_hdr *hdr ) {
    int flags = 0;
    if (isUDP(mSettings)) {
	flags = ntohl(hdr->base.flags);
    } else {
	int n, len;
	char *p = (char *)hdr;
	len = sizeof(client_hdr_v1);
	int sorcvtimer = 0;
	// sorcvtimer units microseconds convert to that
	// minterval double, units seconds
	// mAmount integer, units 10 milliseconds
	// divide by two so timeout is 1/2 the interval
	if (mSettings->mInterval) {
	    sorcvtimer = (int) (mSettings->mInterval * 1e6) / 2;
	} else if (isModeTime(mSettings)) {
	    sorcvtimer = (mSettings->mAmount * 1000) / 2;
	}
	if (sorcvtimer > 0) {
#ifdef WIN32
	    // Windows SO_RCVTIMEO uses ms
	    DWORD timeout = (double) sorcvtimer / 1e3;
#else
	    struct timeval timeout;
	    timeout.tv_sec = sorcvtimer / 1000000;
	    timeout.tv_usec = sorcvtimer % 1000000;
#endif // WIN32
	    if (setsockopt( mSettings->mSock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0 ) {
		WARN_errno( mSettings->mSock == SO_RCVTIMEO, "socket" );
	    }
	}
	// Read the headers but don't pull them from the queue in order to
	// preserve server thread accounting, i.e. these exchanges will
	// be part of traffic accounting
	if ((n = recvn(server->mSock, p, 4, MSG_PEEK)) == 4) {
	    flags = ntohl(hdr->base.flags);
	    len=0;
	    if ((flags & HEADER_EXTEND) != 0) {
		len = sizeof(client_hdr);
	    } else if ((flags & HEADER_VERSION1) != 0) {
		len = sizeof(client_hdr_v1);
	    }
	    if (len && ((n = recvn(server->mSock, p, len, MSG_PEEK)) != len)) {
		flags = 0;
		return -1;
	    }
	}
    }
    if ((flags & HEADER_EXTEND) != 0 ) {
	reporter_peerversion(server, ntohl(hdr->extend.version_u), ntohl(hdr->extend.version_l));
	//  Extended header successfully read. Ack the client with our version info now
	ClientHeaderAck();
    }
    return 1;
}

int Listener::ClientHeaderAck(void) {
    client_hdr_ack ack;
    int sotimer = 0;
    int optflag;
    ack.typelen.type  = htonl(CLIENTHDRACK);
    ack.typelen.length = htonl(sizeof(client_hdr_ack));
    ack.flags = 0;
    ack.reserved1 = 0;
    ack.reserved2 = 0;
    ack.version_u = htonl(IPERF_VERSION_MAJORHEX);
    ack.version_l = htonl(IPERF_VERSION_MINORHEX);
    int rc = 1;
    // This is a version 2.0.10 or greater client
    // write back to the client so it knows the server
    // version
    if (!isUDP(server)) {
	// sotimer units microseconds convert
	if (server->mInterval) {
	    sotimer = (int) ((server->mInterval * 1e6) / 4);
	} else if (isModeTime(server)) {
	    sotimer = (int) ((server->mAmount * 1000) / 4);
	}
	if (sotimer > HDRXACKMAX) {
	    sotimer = HDRXACKMAX;
	} else if (sotimer < HDRXACKMIN) {
	    sotimer = HDRXACKMIN;
	}
#ifdef WIN32
	// Windows SO_RCVTIMEO uses ms
	DWORD timeout = (double) sotimer / 1e3;
#else
	struct timeval timeout;
	timeout.tv_sec = sotimer / 1000000;
	timeout.tv_usec = sotimer % 1000000;
#endif
	if ((rc = setsockopt( server->mSock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout))) < 0 ) {
	    WARN_errno( rc < 0, "setsockopt SO_SNDTIMEO");
	}
	optflag=1;
	// Disable Nagle to reduce latency of this intial message
	if ((rc = setsockopt( server->mSock, IPPROTO_TCP, TCP_NODELAY, (char *)&optflag, sizeof(int))) < 0 ) {
	    WARN_errno(rc < 0, "tcpnodelay" );
	}
    }
    if (isUDP(server) && (server->mBufLen < (int) sizeof(client_hdr_ack))) {
        fprintf( stderr, warn_len_too_small_peer_exchange, "Server", server->mBufLen, sizeof(client_hdr_ack));
    }
    if ((rc = send(server->mSock, (const char*)&ack, sizeof(client_hdr_ack),0)) < 0) {
	WARN_errno( rc <= 0, "send_ack" );
	rc = 0;
    }
    // Re-nable Nagle
    optflag=0;
    if (!isUDP( server ) && (rc = setsockopt( server->mSock, IPPROTO_TCP, TCP_NODELAY, (char *)&optflag, sizeof(int))) < 0 ) {
	WARN_errno(rc < 0, "tcpnodelay" );
    }
    return rc;
}
