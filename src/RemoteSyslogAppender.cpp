/*
 * RemoteSyslogAppender.cpp
 *
 * Copyright 2001, LifeLine Networks BV (www.lifeline.nl). All rights reserved.
 * Copyright 2001, Walter Stroebel. All rights reserved.
 *
 * See the COPYING file for the terms of usage and distribution.
 */

#ifdef WIN32
//  Avoid annoying Microsoft warnings ...
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "PortabilityImpl.hh"

#ifdef LOG4CPP_HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <cstdlib>
#include <stdio.h>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <log4cpp/RemoteSyslogAppender.hh>
#include <log4cpp/FactoryParams.hh>
#include <memory>

#ifndef WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace log4cpp {

    int RemoteSyslogAppender::toSyslogPriority(Priority::Value priority) {
        static int priorities[8] = { LOG_EMERG, LOG_ALERT, LOG_CRIT, LOG_ERR,
                                     LOG_WARNING, LOG_NOTICE, LOG_INFO, 
                                     LOG_DEBUG };
        int result;

        priority++;
        priority /= 100;

        if (priority < 0) {
            result = LOG_EMERG;
        } else if (priority > 7) {
            result = LOG_DEBUG;
        } else {
            result = priorities[priority];
        }

        return result;
    }
        

    RemoteSyslogAppender::RemoteSyslogAppender(const std::string& name, 
                                   const std::string& syslogName, 
                                   const std::string& relayer,
                                   int facility,
                                   int portNumber,
                                   bool tcp) : 
        LayoutAppender(name),
        _syslogName(syslogName),
        _relayer(relayer),
        _facility((facility == -1) ? LOG_USER : facility),
        _portNumber((portNumber == -1) ? 514 : portNumber),
        _socket (INVALID_SOCKET),
        _ipAddr (0),
#ifdef WIN32
        _cludge(false),
        _mutex(INVALID_HANDLE_VALUE),
#else
        _mutex(PTHREAD_MUTEX_INITIALIZER),
#endif
        _tcp (tcp)
    {
#ifdef WIN32
        _mutex = CreateMutex(NULL, FALSE, NULL);
#endif
        open();
    }
    
    RemoteSyslogAppender::~RemoteSyslogAppender() {
        close();
#ifdef WIN32
        CloseHandle(_mutex);
        if (_cludge) {
            // we started it, we end it.
            WSACleanup ();
        }
#endif
    }

    void RemoteSyslogAppender::open() {
        if (!_ipAddr) {
            struct hostent *pent = gethostbyname(_relayer.c_str());
#ifdef WIN32
            if (pent == NULL) {
                if (WSAGetLastError() == WSANOTINITIALISED) {
                    WSADATA wsaData;
                    int err;

                    err = WSAStartup(0x101, &wsaData);
                    if (err) {
                        // loglog("RemoteSyslogAppender: WSAStartup returned %d", err);
                        return; // fail silently
                    }
                    pent = gethostbyname(_relayer.c_str());
                    _cludge = true;
                } else {
                    // loglog("RemoteSyslogAppender: gethostbyname returned error");
                    return; // fail silently
                }
            }
#endif
            if (pent == NULL) {
                in_addr_t ip = inet_addr(_relayer.c_str());
                pent = gethostbyaddr((const char *)&ip, sizeof(in_addr_t), AF_INET);
                if (pent == NULL) {
                    // loglog("RemoteSyslogAppender: failed to resolve host %s", _relayer.c_str());
                    return; // fail silently                    
                }
            }
            _ipAddr = *(in_addr_t*)(pent->h_addr); // fixed bug #1579890
        }

        if (!_tcp) {
            // Get a datagram socket.
            _socket = socket(AF_INET, SOCK_DGRAM, 0);
            if (!IS_VALID_SOCKET(_socket)) {
                // loglog("RemoteSyslogAppender: failed to open socket");
                return; // fail silently                    
            }

        }
    }

    void RemoteSyslogAppender::close() {
#ifdef WIN32
        WaitForSingleObject(_mutex, INFINITE);
#else
        pthread_mutex_lock(&_mutex);
#endif
        if (IS_VALID_SOCKET(_socket)) {
            closesocket(_socket);
            _socket = INVALID_SOCKET;
        }
#ifdef WIN32
        ReleaseMutex(_mutex);
#else
        pthread_mutex_unlock(&_mutex);
#endif
    }

    void RemoteSyslogAppender::_append(const LoggingEvent& event) {
        const std::string message(_getLayout().format(event));
        size_t messageLength = message.length();
        char *buf = new char [messageLength + 16];
        int priority = _facility + toSyslogPriority(event.priority);
        int preambleLength = sprintf (buf, "<%d>", priority);
        memcpy (buf + preambleLength, message.data(), messageLength);

        sockaddr_in sain;
        sain.sin_family = AF_INET;
        sain.sin_port   = htons (_portNumber);
        // NO, do NOT use htonl on _ipAddr. Is already in network order.
        sain.sin_addr.s_addr = _ipAddr;

        if (_tcp) {
#ifdef WIN32
            WaitForSingleObject(_mutex, INFINITE);
#else
            pthread_mutex_lock(&_mutex);
#endif

            for (int attempt = 0; attempt < 3; attempt++) {
                if (!IS_VALID_SOCKET(_socket)) {
                    _socket = socket(AF_INET, SOCK_STREAM, 0);
                    if (IS_VALID_SOCKET(_socket)) {
                        if (connect(_socket, (const sockaddr *)&sain, sizeof sockaddr_in) < 0) {
                            closesocket(_socket);
                            _socket = INVALID_SOCKET;
                        }
                    }
                }
                if (IS_VALID_SOCKET(_socket)) {
                    char octet_framing[10];
                    int octet_framing_length = sprintf(octet_framing, "%u ", (unsigned int)(messageLength + preambleLength));
                    if ((send(_socket, octet_framing, octet_framing_length, 0) == (int)octet_framing_length) &&
                        (send(_socket, buf, (int)messageLength + preambleLength, 0) == (int)messageLength + preambleLength)) {
                        //  Attempt succeeded.  No need to try further ...
                        break;
                    } else {
                        //  Attempt failed.  Socket in unclear state, so to resync
                        //  close it and try with a fresh one ...
                        closesocket(_socket);
                        _socket = INVALID_SOCKET;
                    }
                }
            }

#ifdef WIN32
            ReleaseMutex(_mutex);
#else
            pthread_mutex_unlock(&_mutex);
#endif
        } else {
            while (messageLength > 0) {
                /* if packet larger than maximum (900 bytes), split
                   into two or more syslog packets. */
                if (preambleLength + messageLength > 900) {
                    sendto(_socket, buf, 900, 0, (struct sockaddr *) &sain, sizeof(sain));
                    messageLength -= (900 - preambleLength);
                    std::memmove(buf + preambleLength, buf + 900, messageLength);
                    // note: we might need to sleep a bit here
                } else {
                    sendto(_socket, buf, preambleLength + (int)messageLength, 0, (struct sockaddr *) &sain, sizeof(sain));
                    break;
                }
            }
        }

        delete[] buf;
    }

    bool RemoteSyslogAppender::reopen() {
        close();
        open();
        return true;
    }
    
    std::auto_ptr<Appender> create_remote_syslog_appender(const FactoryParams& params)
    {
       std::string name, syslog_name, relayer;
       int facility = -1, port_number = -1;
       params.get_for("remote syslog appender").required("name", name)("syslog_name", syslog_name)("relayer", relayer)
                                               .optional("facility", facility)("port", port_number);
       return std::auto_ptr<Appender>(new RemoteSyslogAppender(name, syslog_name, relayer, facility, port_number));
    }
}
