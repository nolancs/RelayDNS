//////////////////////////////////////////////////////////////////////////////////
//
// File: Packet.cpp
//
// Desc: DNS Packet object
//
//////////////////////////////////////////////////////////////////////////////////
#include <arpa/inet.h>
#include <iostream>
#include "Error.h"
#include "Packet.h"


//################################################################################
//##
//## Class: DNSPacket
//##
//##  Desc: One big public object that represents a DNS packet and contains some
//##        encoding/decoding mechanisms.
//##
//################################################################################


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::Decode()
//  Description: Decode a DNS packet into this object given raw packet data.
//               This data it decodes must be set via SetRawData().
//      Outputs: Non-zero on error.
//
//////////////////////////////////////////////////////////////////////////////////

int    DNSPacket::Decode()
{
    unsigned char *inData = mRawPacketData;
    unsigned char *inDataStart = mRawPacketData;
    size_t inDataLen = mRawPacketLen;
    size_t inDataLenStart = mRawPacketLen;
    
    // Check for existing data.
    if (!mRawPacketData)
    {
        return -1;
    }
    
    //
    // Decode header
    //
    size_t headerSize = sizeof(DNS_HEADER);
    
    if (inDataLen < headerSize)
        return -1;
    
    memcpy(&mHeader, inData, headerSize);
    inData += headerSize;
    inDataLen -= headerSize;
    
    mHeader.id = ntohs(mHeader.id);
    mHeader.qdcount = ntohs(mHeader.qdcount);
    mHeader.ancount = ntohs(mHeader.ancount);
    mHeader.nscount = ntohs(mHeader.nscount);
    mHeader.arcount = ntohs(mHeader.arcount);
    
    //
    // Decode qname
    //
    if (DNSPacket::DecodeAddrStr(inData, inDataLen, mQuestionName))
        return -1;
    
    //
    // Decode question
    //
    size_t questionSize = sizeof(DNS_QUESTION);
    
    if (inDataLen < questionSize)
        return -1;
    
    memcpy(&mQuestion, inData, questionSize);
    inData += questionSize;
    inDataLen -= questionSize;
    
    mQuestion.qtype = ntohs(mQuestion.qtype);
    mQuestion.qclass = ntohs(mQuestion.qclass);
    
    //int bytesUsed = inData - inDataStart;
    //cout << "Decoded packet. " << bytesUsed << " bytes used, " << inDataLen << " unused.\n";
    
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::DecodeAddrStr()
//  Description: Decode an adress string in the "qname" format.
//       Inputs: ioData (IN/OUT) data buffer
//               ioDataLen (IN/OUT) data buffer length
//               outString (OUT) holds the decoded string
//      Outputs: Non-zero on error.
//          Notes: Static.
//
//////////////////////////////////////////////////////////////////////////////////

int DNSPacket::DecodeAddrStr(
                             unsigned char *&ioData, size_t &ioDataLen, string &outString)
{
    //
    // Decode in qname format
    //
    int sectionLen = 0;
    
    if (ioDataLen < 1)
        return -1;
    sectionLen = *ioData;
    ++ioData;
    --ioDataLen;
    
    outString.clear();
    while (sectionLen != 0)
    {
        // Append section
        if (ioDataLen < sectionLen)
            return -1;
        outString.append((char*)ioData, sectionLen);
        ioData += sectionLen;
        ioDataLen -= sectionLen;
        
        // Find next length
        if (ioDataLen < 1)
            return -1;
        sectionLen = *ioData;
        ++ioData;
        --ioDataLen;
        
        // Add period if we'll be appending more
        if (sectionLen != 0)
            outString += '.';
    }
    
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::Decode()
//  Description: Decode a DNS packet into this object given raw packet data.
//       Inputs: outData (OUT) data buffer to fill
//               outDataLen (OUT) amount written into the data buffer
//               inRemainsLen (OUT) length of data buffer (remaining length.)
//      Outputs: Non-zero on error.
//
//////////////////////////////////////////////////////////////////////////////////

int DNSPacket::Encode(
                      unsigned char *&outData, size_t &outDataLen, size_t inRemainsLen)
{
    //
    // Encode header
    //
    size_t headerSize = sizeof(DNS_HEADER);
    
    if (inRemainsLen < headerSize)
        return -1;
    
    mHeader.id = htons(mHeader.id);
    mHeader.qdcount = htons(mHeader.qdcount);
    mHeader.ancount = htons(mHeader.ancount);
    mHeader.nscount = htons(mHeader.nscount);
    mHeader.arcount = htons(mHeader.arcount);
    
    memcpy(outData, &mHeader, sizeof(DNS_HEADER));
    outData += headerSize;
    outDataLen += headerSize;
    inRemainsLen -= headerSize;
    
    mHeader.id = ntohs(mHeader.id);
    mHeader.qdcount = ntohs(mHeader.qdcount);
    mHeader.ancount = ntohs(mHeader.ancount);
    mHeader.nscount = ntohs(mHeader.nscount);
    mHeader.arcount = ntohs(mHeader.arcount);
    
    //
    // Encode qname
    //
    if (DNSPacket::EncodeAddrStr(outData, outDataLen, inRemainsLen, mQuestionName))
        return -1;
    
    //
    // Encode question
    //
    size_t questionSize = sizeof(DNS_QUESTION);
    
    if (inRemainsLen < questionSize)
        return -1;
    
    mQuestion.qtype = htons(mQuestion.qtype);
    mQuestion.qclass = htons(mQuestion.qclass);
    
    memcpy(outData, &mQuestion, questionSize);
    outData += questionSize;
    outDataLen += questionSize;
    inRemainsLen -= questionSize;
    
    mQuestion.qtype = ntohs(mQuestion.qtype);
    mQuestion.qclass = ntohs(mQuestion.qclass);
    
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::EncodeAddrStr()
//  Description: Encode an adress string in the "qname" format.
//       Inputs: ioData (IN/OUT) data buffer
//               ioDataLen (IN/OUT) data buffer current length
//               ioRemainsLen (OUT) data buffer remaining length
//               inString (IN) String to encode
//      Outputs: Non-zero on error.
//          Notes: Static.
//
//////////////////////////////////////////////////////////////////////////////////

int DNSPacket::EncodeAddrStr(
                             unsigned char *&ioData, size_t &ioDataLen, size_t &ioRemainsLen,
                             string &inString)
{
    int i, sectionLen, sectionStart = 0, sectionEnd;
    
    //
    // Encode sections of the string, delimited by '.'
    //
    while ((sectionEnd = inString.find('.', sectionStart)) != string::npos)
    {
        // Length
        sectionLen = sectionEnd - sectionStart;
        if (ioRemainsLen < 1 + sectionLen)
            return -1;
        
        *ioData = sectionLen;
        ++ioData;
        ++ioDataLen;
        --ioRemainsLen;
        
        // Copy section
        memcpy(ioData, &(inString.data()[sectionStart]), sectionLen);
        ioDataLen += sectionLen;
        ioRemainsLen -= sectionLen;
        
        // Skip '.'
        sectionStart = sectionEnd + 1;
    }
    
    //
    // Last section
    //
    // Length
    sectionLen = inString.size() - sectionStart;
    if (ioRemainsLen < 2 + sectionLen)
        return -1;
    
    *ioData = sectionLen;
    ++ioData;
    ++ioDataLen;
    --ioRemainsLen;
    
    // Copy section
    memcpy(ioData, &(inString.data()[sectionStart]), sectionLen);
    ioDataLen += sectionLen;
    ioRemainsLen -= sectionLen;
    
    // Final zero to terminate
    *ioData = 0;
    ++ioData;
    ++ioDataLen;
    --ioRemainsLen;
    
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::Print()
//  Description: Print packet contents
//
//////////////////////////////////////////////////////////////////////////////////

void DNSPacket::Print()
{
    cout << "Packet Contents,\n"
    "\t id: " << mHeader.id << "\n"
    "\t recursion_desired: " << mHeader.rd << "\n"
    "\t truncated message: " << mHeader.tc << "\n"
    "\t authoritive_answer: " << mHeader.aa << "\n"
    "\t opcode: " << mHeader.opcode << "\n"
    "\t response_flag: " << mHeader.resp << "\n"
    "\t response_code: " << mHeader.rcode << "\n"
    "\t recursion_available: " << mHeader.ra << "\n"
    "\t question_entry_count: " << mHeader.qdcount << "\n"
    "\t answer_entry_count: " << mHeader.qdcount << "\n"
    "\t authority_entry_count: " << mHeader.qdcount << "\n"
    "\t resource_entry_count: " << mHeader.qdcount << "\n"
    "\t question_name: " << mQuestionName << "\n"
    "\t question_type: " << mQuestion.qtype << "\n"
    "\t question_class: " << mQuestion.qclass << "\n";
    fflush(stdout);
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::SetRawData()
//  Description: Set the raw data used to decode the packet. Copies the memory.
//       Inputs: inData (IN) data
//               inLen (IN) data length
//      Outputs: Non-zero on error.
//
//////////////////////////////////////////////////////////////////////////////////

int DNSPacket::SetRawData(unsigned char* inData, size_t inLen)
{
    if (mRawPacketData)
    {
        free(mRawPacketData);
        mRawPacketData = nullptr;
        mRawPacketLen = 0;
    }
    mRawPacketLen = inLen;
    mRawPacketData = (unsigned char*) malloc(mRawPacketLen);
    memcpy(mRawPacketData, inData, mRawPacketLen);
    
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::SetRawPacketID()
//  Description: Set packet id in the raw data, not the decoded data.
//       Inputs: inID (IN) new id.
//      Outputs: Non-zero on error.
//
//////////////////////////////////////////////////////////////////////////////////

int DNSPacket::SetRawPacketID(unsigned short inID)
{
    if (!mRawPacketData)
    {
        ReportError("No raw packet data set");
        return -1;
    }
    
    inID = htons(inID);
    memcpy(mRawPacketData, &inID, sizeof(unsigned short));
    
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: DNSPacket::SetRawPacketID()
//  Description: Get packet id in the raw data, not the decoded data. This just
//               reads the first 16 bits of the packet then translates it from
//               network byte order.
//       Inputs: outID (OUT) packet id will be stored here.
//      Outputs: Non-zero on error.
//
//////////////////////////////////////////////////////////////////////////////////

int DNSPacket::GetRawPacketID(unsigned short& outID)
{
    if (!mRawPacketData)
    {
        ReportError("No raw packet data set");
        return -1;
    }
    
    unsigned short userPacketId = 0;
    memcpy(&userPacketId, mRawPacketData, sizeof(unsigned short));
    outID = ntohs(userPacketId);
    
    return 0;
}



