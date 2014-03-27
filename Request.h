//////////////////////////////////////////////////////////////////////////////////
//
// File: Request.h
//
// Desc:
//
//////////////////////////////////////////////////////////////////////////////////
#ifndef REQUEST_H
#define REQUEST_H
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string>
#include <memory>
#include <chrono>
#include "Packet.h"

using namespace std;


//################################################################################
//##
//## Class: Request
//##
//##  Desc: Represents a request sent to the server. Used to store data as the
//##        request is processed through various stages.
//##
//################################################################################

class Request
{
public:
    Request()
    : mClientPacketID(0),
    mOurPacketID(0)
    {
    }
    virtual ~Request()
    {
    }
    
    DNSPacket                                   mPacket;
    struct sockaddr_in                          mClientAddr;
    unsigned short                              mClientPacketID;
    unsigned short                              mOurPacketID;
    string                                      mDomainName;
    chrono::high_resolution_clock::time_point   mForwardedTime;
};

#endif
