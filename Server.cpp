//////////////////////////////////////////////////////////////////////////////////
//
// File: Server.cpp
//
// Desc: Server and ServerThread classes.
//
//////////////////////////////////////////////////////////////////////////////////
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <iostream>
#include "Server.h"
#include "Request.h"
#include "Packet.h"
#include "Error.h"

using namespace std;


//################################################################################
//##
//## Class: Server
//##
//##  Desc: Main server representation object.
//##
//################################################################################


// Static class variables
condition_variable  Server::sShuttingDownCV;
mutex               Server::sShuttingDownCVMutex;


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::Server()
//  Description: Constructor.
//       Inputs: inListenPort (IN) the port to listen on
//               inFwdStr (IN) the remote DNS server name we'll be forwarding to
//               inFwdPort (IN) the remote DNS server port
//
//////////////////////////////////////////////////////////////////////////////////

Server::Server(unsigned short inListenPort, const char* inFwdStr,
               unsigned short inFwdPort)
: mServerPort(inListenPort),
  mServerSocket(-1),
  mFwdStr(inFwdStr),
  mFwdPort(inFwdPort),
  mFwdSocket(-1),
  mGenIDCounter(0),
  mInboxQueueSemaphore(nullptr),
  mOutboxSemaphore(nullptr),
  mMaintainenceThread(nullptr)
{
    if (!(mInboxQueueSemaphore = sem_open("mInboxQueueSemaphore", O_CREAT, 0666, 0)))
    {
        ReportError("sem_open(mInboxQueueSemaphore) failed");
    }
    
    if (!(mOutboxSemaphore = sem_open("mOutboxSemaphore", O_CREAT, 0666, 0)))
    {
        ReportError("sem_open(mOutboxSemaphore) failed");
    }
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::~Server()
//  Description: Destructor.
//
//////////////////////////////////////////////////////////////////////////////////

Server::~Server()
{
    // Clean up sockets
    if (mFwdSocket != -1)
    {
        close(mFwdSocket);
        mFwdSocket = -1;
    }
    
    if (mServerSocket)
    {
        close(mServerSocket);
        mServerSocket = -1;
    }
    
    // Clean up semaphores
    if (mInboxQueueSemaphore && sem_close(mInboxQueueSemaphore))
    {
        ReportError("sem_close(mInboxQueueSemaphore) failed");
        mInboxQueueSemaphore = nullptr;
    }
    
    if (mOutboxSemaphore && sem_close(mOutboxSemaphore))
    {
        ReportError("sem_close(mOutboxSemaphore) failed");
        mOutboxSemaphore = nullptr;
    }
    
    // Clean up ServerThread memory
    for (auto stObj : mInboxThreads)
        delete stObj;
    mInboxThreads.clear();
    
    for (auto stObj : mProcessThreads)
        delete stObj;
    mProcessThreads.clear();
    
    for (auto stObj : mOutboxThreads)
        delete stObj;
    mOutboxThreads.clear();
    
    if (mMaintainenceThread)
    {
        delete mMaintainenceThread;
        mMaintainenceThread = nullptr;
    }
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::RunServer()
//  Description: Start and run the server. This function returns when the server
//               has been shut down. It may be shut down by sending signals to
//               the process id. Ctrl+C works.
//      Returns: Non-zero on error.
//
//////////////////////////////////////////////////////////////////////////////////

int Server::RunServer()
{
    //
    // Create signal handlers
    //
    // To gracefully shutdown the server call:
    //        kill -s TERM <pid>
    //
    signal(SIGINT, Server::HandleSignal);
    signal(SIGILL, Server::HandleSignal);
    signal(SIGTERM, Server::HandleSignal);
    signal(SIGABRT, Server::HandleSignal);
    
    //
    // Setup remote DNS server structures
    //
    socklen_t addrLen = sizeof(struct sockaddr_in);
    
    mFwdSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (mFwdSocket == -1)
    {
        ReportError("Could not create socket, errno %d", errno);
        return -1;
    }
    
    struct hostent *host = gethostbyname(mFwdStr.c_str());
    if (!host)
    {
        ReportError("Failed to resolve address of forward DNS server %s",
                    mFwdStr.c_str());
        return -1;
    }
    memset(&mFwdSocketAddr, 0, sizeof(mFwdSocketAddr));
    mFwdSocketAddr.sin_family = AF_INET;
    mFwdSocketAddr.sin_port = htons(mFwdPort);
    memcpy(&mFwdSocketAddr.sin_addr, host->h_addr_list[0], host->h_length);
    
    //
    // Create 'Inbox' socket and listen
    //
    mServerSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (mServerSocket == -1)
    {
        ReportError("Could not create socket, errno %d", errno);
        return -1;
    }
    
    memset(&mServerSocketAddr, 0, sizeof(mServerSocketAddr));
    mServerSocketAddr.sin_family = AF_INET;
    mServerSocketAddr.sin_addr.s_addr = INADDR_ANY;
    mServerSocketAddr.sin_port = htons(mServerPort);
    int rc = ::bind(mServerSocket, (struct sockaddr*)&mServerSocketAddr, addrLen);
    if (rc != 0)
    {
        ReportError("Could not listen on port %d, errno %d", mServerPort, errno);
        return -1;
    }
    
    //
    // Spawn threads (scaleCount times each)
    //
    ServerThreadMaintainence *stMaintainence = nullptr;
    ServerThreadOutbox *stOutbox = nullptr;
    ServerThreadProcess *stProcess = nullptr;
    ServerThreadInbox *stInbox = nullptr;
    thread *stThread = nullptr;
    int scaleCount = 1;
    
    // We only ever need one maintainence thread
    stMaintainence = new ServerThreadMaintainence(this);
    stThread = new thread(&ServerThreadMaintainence::ThreadMain, stMaintainence);
    stMaintainence->SetThread(stThread);
    mMaintainenceThread = stMaintainence;
    
    for (int i = 0; i < scaleCount; ++i)
    {
        // Start them up in reverse order
        stOutbox = new ServerThreadOutbox(this);
        stThread = new thread(&ServerThreadOutbox::ThreadMain, stOutbox);
        stOutbox->SetThread(stThread);
        mOutboxThreads.push_back(stOutbox);
        
        stProcess = new ServerThreadProcess(this);
        stThread = new thread(&ServerThreadProcess::ThreadMain, stProcess);
        stProcess->SetThread(stThread);
        mProcessThreads.push_back(stProcess);
        
        stInbox = new ServerThreadInbox(this);
        stThread = new thread(&ServerThreadInbox::ThreadMain, stInbox);
        stInbox->SetThread(stThread);
        mInboxThreads.push_back(stInbox);
    }
    
    printf("DNS server started:\n\tPort: %d\n\tForwarding: %s:%d\n\n",
           (int)mServerPort, mFwdStr.c_str(), (int)mFwdPort);
    fflush(stdout);
    
    //
    // Wait for shutdown signal (via condition_variable)
    //
    unique_lock<mutex> shutdownLock(sShuttingDownCVMutex);
    sShuttingDownCV.wait(shutdownLock);
    this->mShuttingDown = true;
    printf("Shutting down...\n");
    fflush(stdout);
    
    //
    // Shutdown threads:
    // All of our threads block on sem_wait or recvfrom. These are cancellation points.
    // The threads default to the PTHREAD_CANCEL_DEFERRED, meaning they can only cancel
    // while inside of one of these cancellation points. This should protect us from
    // memory leaks during the processing/handling stage of each thread.
    // pthread_setcancelstate(PTHREAD_CANCEL_ENABLE/DISABLE) are also wrapped around the
    // processing stage in case that code calls any other cancellation points.
    // Joins all the cancelled threads to cleanup their memory (stack and descriptor.)
    //
    printf("Shutting down threads...\n");
    for (auto stObj : mInboxThreads)
    {
        if (stObj->GetThread())
        {
            pthread_cancel(stObj->GetThread()->native_handle());
            stObj->GetThread()->join();
        }
    }
    for (auto stObj : mProcessThreads)
    {
        if (stObj->GetThread())
        {
            pthread_cancel(stObj->GetThread()->native_handle());
            stObj->GetThread()->join();
        }
    }
    for (auto stObj : mOutboxThreads)
    {
        if (stObj->GetThread())
        {
            pthread_cancel(stObj->GetThread()->native_handle());
            stObj->GetThread()->join();
        }
    }
    pthread_cancel(mMaintainenceThread->GetThread()->native_handle());
    mMaintainenceThread->GetThread()->join();
    printf("Shutting down threads: complete.\n");
    
    //
    // Print stats
    //
    int packetsIn = mStatsPacketsIn;
    int packetsOut = mStatsPacketsOut;
    int requests = mStatsRequests;
    int served = mStatsServed;
    int timeOuts = mStatsTimeOuts;
    int processing = mStatsRequests - (mStatsServed+mStatsTimeOuts);
    printf("\nStatistics:\n\t");
    printf("PacketsIn(%d), PacketsOut(%d), Requests(%d), Served(%d), TimeOuts(%d), Processing(%d)\n\n",
           packetsIn, packetsOut, requests, served, timeOuts, processing);
    fflush(stdout);
    
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::HandleSignal()
//  Description: Handle any signal tells us to shut down.
//        Input: Signal
//
//////////////////////////////////////////////////////////////////////////////////

void Server::HandleSignal(int inSig)
{
    printf("Received signal %d, shutting down...\n", inSig);
    fflush(stdout);
    sShuttingDownCV.notify_all();
    
    // Reset signal handlers in case the shutdown stalled.
    signal(SIGINT, nullptr);
    signal(SIGILL, nullptr);
    signal(SIGTERM, nullptr);
    signal(SIGABRT, nullptr);
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::InboxQueuePopFront()
//  Description: Push the next Request object onto the Inbox queue.
//        Input: inReq (IN) the Request.
//      Returns: Non-zero on failure.
//
//////////////////////////////////////////////////////////////////////////////////

int Server::InboxQueuePushBack(unique_ptr<Request> inReq)
{
    mInboxQueueMutex.lock();
    mInboxQueue.push(move(inReq));
    ++mStatsPacketsIn;
    mInboxQueueMutex.unlock();
    if (sem_post(mInboxQueueSemaphore))
    {
        ReportError("sem_post(mInboxQueueSemaphore) failed");
        return -1;
    }
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::InboxQueuePopFront()
//  Description: Pop the next Request object off the Inbox queue.
//      Returns: The Request object or nullptr.
//
//////////////////////////////////////////////////////////////////////////////////

unique_ptr<Request> Server::InboxQueuePopFront()
{
    mInboxQueueMutex.lock();
    if (!mInboxQueue.front())
    {
        mInboxQueueMutex.unlock();
        return nullptr;
    }
    unique_ptr<Request> outReq(move(mInboxQueue.front()));
    mInboxQueue.pop();
    mInboxQueueMutex.unlock();
    return move(outReq);
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::InboxQueueWaitForData()
//  Description: Block until Requests arrive in the Inbox.
//      Returns: Non-zero on failure.
//
//////////////////////////////////////////////////////////////////////////////////

int Server::InboxQueueWaitForData()
{
    if (sem_wait(mInboxQueueSemaphore))
    {
        ReportError("sem_wait() failed");
        return -1;
    }
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::GenerateUniqueID()
//  Description: Generate an ID unique to this server since we may be passing
//               requests through that contain possible duplicate IDs.
//      Returns: the ID.
//
//////////////////////////////////////////////////////////////////////////////////

unsigned short Server::GenerateUniqueID()
{
    // This could obviously be improved upon to create less predictable IDs
    unsigned short idOut;
    mGenIDMutex.lock();
    if (++mGenIDCounter == USHRT_MAX)
        mGenIDCounter = 1;
    idOut = mGenIDCounter;
    mGenIDMutex.unlock();
    return idOut;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::OutboxAdd()
//  Description: Add a request to the Outbox.
//       Inputs: inReq (IN) the Request object.
//      Returns: Non-zero on failure.
//
//////////////////////////////////////////////////////////////////////////////////


int Server::OutboxAdd(unique_ptr<Request> inReq)
{
    mOutboxMutex.lock();
    inReq->mForwardedTime = chrono::high_resolution_clock::now();
    mOutboxQueue.push(inReq->mOurPacketID);
    mOutboxArray[inReq->mOurPacketID] = move(inReq);
    mOutboxMutex.unlock();
    if (sem_post(mOutboxSemaphore))
    {
        ReportError("sem_post(mOutboxSemaphore) failed");
        return -1;
    }
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::OutboxRemove()
//  Description: Remove a Request from the Outbox.
//        Input: inID (IN) the packet ID (ours) of the Request.
//      Returns: The Request object on success or nullptr if it didn't exist.
//
//////////////////////////////////////////////////////////////////////////////////

unique_ptr<Request> Server::OutboxRemove(unsigned short inID)
{
    mOutboxMutex.lock();
    unique_ptr<Request>& storedAtID = mOutboxArray[inID];
    if (storedAtID.get() == nullptr)
    {
        mOutboxMutex.unlock();
        return nullptr;
    }
    unique_ptr<Request> outReq(move((storedAtID)));
    mOutboxMutex.unlock();
    return move(outReq);
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::OutboxTimeout()
//  Description: Actively remove Requests from the Outbox that are over the server
//               timeout limit. For the active method we use a separate queue
//               (which is ordered by time) to quickly identify only the timed out
//               packets.
//
//               All Requests check their timeout before responding, so nothing
//               goes back to the client that is outside the timeout window. This
//               method just cleans up those timeouts actively instead of waiting
//               for a response or ID re-use to do it passively.
//
//////////////////////////////////////////////////////////////////////////////////

void Server::OutboxTimeout()
{
    mOutboxMutex.lock();
    chrono::high_resolution_clock::time_point rightNow = chrono::high_resolution_clock::now();
    Request* oldestReq = nullptr;
    unsigned short oldestReqID = 0;
    
    while (!mOutboxQueue.empty())
    {
        oldestReqID = mOutboxQueue.front();
        
        // Check that this entry exists in the mOutboxArray still
        unique_ptr<Request> &storedAtID = mOutboxArray[oldestReqID];
        if ((oldestReq = storedAtID.get()) == nullptr)
        {
            // This Request has already been processed, move on
            mOutboxQueue.pop();
            continue;
        }
        
        // If this entry hasn't timed out, none of the newer entries above it have either
        long elapsedMS = chrono::duration_cast<chrono::milliseconds>(rightNow-oldestReq->mForwardedTime).count();
        if (elapsedMS < SERVER_TIMEOUT_MS)
            break;
        
        // Timed out, remove it and delete it from the mOutboxArray
#if SERVER_VERBOSE
        printf(">> Timeout(Active): %s, took %ld ms (max %d)\n", oldestReq->mDomainName.c_str(),
               elapsedMS, SERVER_TIMEOUT_MS);
        fflush(stdout);
#endif
        delete storedAtID.release();
        oldestReq = nullptr;
        ++mStatsTimeOuts;
        
        // Continue checking the next oldest entry
        mOutboxQueue.pop();
    }
    
    mOutboxMutex.unlock();
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::InboxQueueWaitForData()
//  Description: Block until Requests arrive in the Outbox.
//        Returns: Non-zero on failure.
//
//////////////////////////////////////////////////////////////////////////////////

int Server::OutboxWaitForData()
{
    if (sem_wait(mOutboxSemaphore))
    {
        ReportError("sem_wait(mOutboxSemaphore) failed");
        return -1;
    }
    return 0;
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::AddToCacheMap()
//  Description: Super simple caching mechanism. Add to it. This was just for
//               experimenting. Obviously you'd need something more intelligent
//               that has TTL values and keeps itself from growing infinitely.
//       Inputs: inDomain (IN) domain name string to cache.
//               inPacket (IN) the packet data to cache. This will be copied.
//      Returns: Non-zero on failure.
//
//////////////////////////////////////////////////////////////////////////////////
#if SERVER_USE_CACHE
int Server::AddToCacheMap(string inDomain, pair<unsigned char*, size_t>& inPacket)
{
    mCacheMapMutex.lock();
    if (mCacheMap.find(inDomain) != mCacheMap.end())
    {
        mCacheMapMutex.unlock();
        return -1;
    }
    pair<unsigned char*, size_t> newPacket;
    newPacket.second = inPacket.second;
    newPacket.first = (unsigned char*) malloc(inPacket.second);
    memcpy(newPacket.first, inPacket.first, inPacket.second);
    mCacheMap[inDomain] = newPacket;
    mCacheMapMutex.unlock();
    return 0;
}
#endif


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: Server::CheckCacheMap()
//  Description: Super simple caching mechanism. Query it.
//       Inputs: inDomain (IN) domain name string to search for.
//               outPacket (OUT) the cached packet result if found.
//      Returns: True if it found a cache hit.
//
//////////////////////////////////////////////////////////////////////////////////
#if SERVER_USE_CACHE
bool Server::CheckCacheMap(string inDomain, pair<unsigned char*, size_t>& outPacket)
{
    mCacheMapMutex.lock();
    auto found = mCacheMap.find(inDomain);
    if (found != mCacheMap.end())
    {
        outPacket = found->second;
        mCacheMapMutex.unlock();
        return true;
    }
    mCacheMapMutex.unlock();
    return false;
}
#endif


//################################################################################
//##
//## Class: ServerThreadInbox
//##
//##  Desc: Reads packets off InboxPort (53 generally) and adds them to the
//##        the inbox queue.
//##
//################################################################################


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ServerThreadInbox::ThreadMain()
//  Description: Main thread entry point.
//
//////////////////////////////////////////////////////////////////////////////////

void ServerThreadInbox::ThreadMain()
{
    socklen_t addrLen = sizeof(struct sockaddr_in);
    int serverSocket = mServer->GetServerSocket();
    unsigned char buffer[SERVER_BUFFER_SIZE];
    struct sockaddr_in recvAddress;
    int nbytes;
    
    while (!mServer->ShuttingDown())
    {
        nbytes = recvfrom(serverSocket, (char*)buffer, SERVER_BUFFER_SIZE, 0,
                          (struct sockaddr*) &recvAddress, &addrLen);
        if (nbytes <= 0)
        {
            continue;
        }
        
        // Process Packet
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr);
        try
        {
            if (this->HandlePacket(buffer, nbytes, &recvAddress))
            {
                ReportError("Error handling packet");
            }
        }
        catch (...)
        {
            ReportError("Caught exception");
        }
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    }
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ServerThreadInbox::HandlePacket()
//  Description: Minimal processing is done at this stage, this may not even be
//               a valid packet. We simply copy the data and queue it up for the
//               processing thread to look at; leaving more time to read new
//               packets on the Inbox thread.
//
//////////////////////////////////////////////////////////////////////////////////

int ServerThreadInbox::HandlePacket(
                                    unsigned char *inData, size_t inLen, struct sockaddr_in *inFrom)
{
    // Enforce max packet size
    if (inLen > SERVER_MAX_PACKET_SIZE)
    {
        ReportError("Packet too large (%d bytes), discarded.", (int)inLen);
        return 0;
    }
    
    // Add it
    unique_ptr<Request> newReq(new Request());
    newReq->mPacket.SetRawData(inData, inLen);
    memcpy(&newReq->mClientAddr, inFrom, sizeof(struct sockaddr_in));
    this->mServer->InboxQueuePushBack(move(newReq));
    
    return 0;
}


//################################################################################
//##
//## Class: ServerThreadProcess
//##
//##  Desc: Pops Request packets off the inbox queue and processes them. They are
//##        handled (caching) or the packet is forwarded to the remote/forward
//##        DNS server and this Request is moved into the Outbox.
//##
//################################################################################


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ServerThreadProcess::ThreadMain()
//  Description: Main thread entry point.
//
//////////////////////////////////////////////////////////////////////////////////

void ServerThreadProcess::ThreadMain()
{
    while (!mServer->ShuttingDown())
    {
        if (mServer->InboxQueueWaitForData())
        {
            // We may be shutting down now
            continue;
        }
        
        // Processing Request
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr);
        try
        {
            unique_ptr<Request> req(mServer->InboxQueuePopFront());
            if (req.get() == nullptr)
                continue;
            
            if (this->HandleRequest(move(req)))
            {
                ReportError("Error handling request");
            }
        }
        catch (...)
        {
            ReportError("Caught exception");
        }
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    }
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ServerThreadProcess::HandleRequest()
//  Description:
//
//////////////////////////////////////////////////////////////////////////////////

int ServerThreadProcess::HandleRequest(unique_ptr<Request> inReq)
{
    //
    // Decode the packet
    //
    Request* reqPtr = inReq.get();
    int rc = reqPtr->mPacket.Decode();
    if (rc)
    {
        ReportError("Error decoding packet");
        return -1;
    }
    
    //cout << "ServerThreadProcess::HandleRequest()\n";
    //reqPtr->mPacket.Print();
    
    //
    // Check packet validity and set domain
    //
    if (reqPtr->mPacket.mHeader.resp)
    {
        // This is a response packet, we're only supposed to see question
        // packets here. Ignore it.
        ReportError("Response packet found where question packet expected");
        return -1;
    }
    reqPtr->mDomainName = reqPtr->mPacket.mQuestionName;
    ++mServer->mStatsRequests;
    
#if SERVER_USE_CACHE
    //
    // Check for a cached response
    //
    pair<unsigned char*, size_t> packetOut;
    bool found = mServer->CheckCacheMap(reqPtr->mDomainName, packetOut);
    if (found)
    {
        //
        // Send reply to original client
        //
        unsigned short clientPacketId = 0;
        reqPtr->mPacket.GetRawPacketID(clientPacketId);
        int serverSocket = mServer->GetServerSocket();
        size_t addrLen = sizeof(struct sockaddr_in);
        struct sockaddr_in *clientAddress = &reqPtr->mClientAddr;
        unsigned char *data = packetOut.first;
        size_t dataLen = packetOut.second;
        
        clientPacketId = htons(clientPacketId);
        memcpy(data, &clientPacketId, sizeof(clientPacketId));
        
        ++mServer->mStatsServed;
        ++mServer->mStatsPacketsOut;
        if (sendto(serverSocket, data, dataLen, 0,
                   (struct sockaddr*)clientAddress, addrLen) < 0)
        {
            ReportError("sendto client failed");
        }
#if SERVER_VERBOSE
        printf(">> Processed: %s (using Cache)\n", reqPtr->mDomainName.c_str());
        fflush(stdout);
#endif
        return 0;
    }
#endif
    
    //
    // Replace packet id with our own
    //
    unsigned short ourPacketId = mServer->GenerateUniqueID();
    unsigned short clientPacketId = 0;
    
    if (reqPtr->mPacket.GetRawPacketID(clientPacketId))
    {
        ReportError("Failed to get raw packet id");
        return -1;
    }
    if (reqPtr->mPacket.SetRawPacketID(ourPacketId))
    {
        ReportError("Failed to set raw packet id");
        return -1;
    }
    reqPtr->mClientPacketID = clientPacketId;
    reqPtr->mOurPacketID = ourPacketId;
#if SERVER_VERBOSE
    printf("Processing remote DNS request (%s) their_id(%u) our_id(%d)\n",
           reqPtr->mDomainName.c_str(), reqPtr->mClientPacketID,
           reqPtr->mOurPacketID);
    fflush(stdout);
#endif
    
    //
    // Add to outbox
    //
    mServer->OutboxAdd(move(inReq));
    
    //
    // Forward to DNS server
    //
    size_t addrLen = sizeof(struct sockaddr_in);
    int fwdSocket = mServer->GetFwdSocket();
    const struct sockaddr_in* fwdSocketAddr = mServer->GetFwdSocketAddr();
    unsigned char *buffer = reqPtr->mPacket.mRawPacketData;
    size_t nbytes = reqPtr->mPacket.mRawPacketLen;
    
    ++mServer->mStatsPacketsOut;
    if (sendto(fwdSocket, buffer, nbytes, 0,
               (struct sockaddr*)fwdSocketAddr, addrLen) < 0)
    {
        ReportError("sendto fwd dns server failed (fwdSocket: %d, data_size: %u)",
                    fwdSocket, nbytes);
        return -1;
    }
    
    return 0;
}


//################################################################################
//##
//## Class: ServerThreadOutbox
//##
//##  Desc: Waits for replies from the remote/forward DNS server. When received
//##        it sends the reply to the original client. It also handles timeouts.
//##
//################################################################################


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ServerThreadOutbox::ThreadMain()
//  Description: Main thread entry point.
//
//////////////////////////////////////////////////////////////////////////////////

void ServerThreadOutbox::ThreadMain()
{
    socklen_t addrLen = sizeof(struct sockaddr_in);
    int fwdSocket = mServer->GetFwdSocket();
    int serverSocket = mServer->GetServerSocket();
    unsigned char buffer[SERVER_BUFFER_SIZE];
    const struct sockaddr_in *clientAddress = nullptr;
    const struct sockaddr_in *fwdAddress = mServer->GetFwdSocketAddr();
    struct sockaddr_in recvAddress;
    int rc, nbytes;
    
    while (!mServer->ShuttingDown())
    {
        nbytes = recvfrom(fwdSocket, (char*)buffer, SERVER_BUFFER_SIZE, 0,
                          (struct sockaddr*) &recvAddress, &addrLen);
        if (nbytes < 0)
        {
            // We may be shutting down now
            continue;
        }
        
        // Processing Packet
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr);
        try
        {
            if (this->HandlePacket(buffer, nbytes, &recvAddress))
            {
                ReportError("Error handling packet");
            }
        }
        catch (...)
        {
            ReportError("Caught exception");
        }
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    }
}


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ServerThreadOutbox::HandlePacket()
//  Description: Handle a fwd dns server response and send it to the client.
//       Inputs: inData (IN) packet data. Will be copied.
//               inLen (IN) length of packet data.
//               inFrom (IN) address this packet came from.
//      Returns: Non-zero on failure.
//
//////////////////////////////////////////////////////////////////////////////////

int ServerThreadOutbox::HandlePacket(
                                     unsigned char *inData, size_t inLen, struct sockaddr_in *inFrom)
{
    // Enforce max packet size
    if (inLen > SERVER_MAX_PACKET_SIZE)
    {
        ReportError("Packet too large (%d bytes), discarded.", (int)inLen);
        return 0;
    }
    
    // Process Packet
    socklen_t addrLen = sizeof(struct sockaddr_in);
    const struct sockaddr_in *fwdAddress = mServer->GetFwdSocketAddr();
    const struct sockaddr_in *clientAddress = nullptr;
    int serverSocket = mServer->GetServerSocket();
    int rc, nbytes;
    
    //
    // Security check: we should only receive packets from fwd dns ip
    // address and on the correct port. Anything else is fishy.
    //
    if (fwdAddress->sin_addr.s_addr != inFrom->sin_addr.s_addr ||
        fwdAddress->sin_port != inFrom->sin_port)
    {
        unsigned short p1 = ntohs(inFrom->sin_port);
        unsigned short p2 = ntohs(fwdAddress->sin_port);
        unsigned long addr1 = ntohl(inFrom->sin_addr.s_addr);
        unsigned long addr2 = ntohl(fwdAddress->sin_addr.s_addr);
        unsigned char *a1 = (unsigned char*) &addr1;
        unsigned char *a2 = (unsigned char*) &addr2;
        ReportError("Reply from unexpected source: "
                    "%d.%d.%d.%d#%u, expected %d.%d.%d.%d#%u, ignoring",
                    a1[0], a1[1], a1[2], a1[3], p1,
                    a2[0], a2[1], a2[2], a2[3], p2);
        return -1;
    }
    
    //
    // Get our packet ID
    //
    DNSPacket packet;
    unsigned short ourID = 0;
    
    packet.SetRawData(inData, inLen);
    if (packet.GetRawPacketID(ourID))
    {
        ReportError("Unable to get packet id");
        return -1;
    }
    
    //
    // Make sure this is a response packet
    //
    packet.Decode();
    if (!packet.mHeader.resp)
    {
        // This is a question. (resp set to 0)
        ReportError("Outbox received a question (id %u), ignoring", ourID);
        return -1;
    }
    ++mServer->mStatsPacketsIn;
    
    //
    // Lookup initial request
    //
    unique_ptr<Request> thisReq(mServer->OutboxRemove(ourID));
    if (thisReq.get() == nullptr)
    {
        // This Request may have timed out, normal case.
        return 0;
    }
    
    //
    // Calculate elapsed time
    //
    chrono::high_resolution_clock::time_point rightNow = chrono::high_resolution_clock::now();
    long elapsedMS = chrono::duration_cast<chrono::milliseconds>(rightNow-thisReq->mForwardedTime).count();
    
    //
    // Passive timeout
    //
    // Check if the response came fast enough, otherwise discard.
    if (elapsedMS >= SERVER_TIMEOUT_MS)
    {
#if SERVER_VERBOSE
        ++mServer->mStatsTimeOuts;
        printf(">> Timeout(Passive): %s, took %ld ms (max %d)\n", thisReq->mDomainName.c_str(),
               elapsedMS, SERVER_TIMEOUT_MS);
        fflush(stdout);
#endif
        return 0;
    }
    
    //
    // Send reply to original client
    //
    ++mServer->mStatsServed;
    ++mServer->mStatsPacketsOut;
    packet.SetRawPacketID(thisReq->mClientPacketID);
    //packet.Print();
    clientAddress = &thisReq->mClientAddr;
    if (sendto(serverSocket, packet.mRawPacketData, packet.mRawPacketLen, 0,
               (struct sockaddr*)clientAddress, addrLen) < 0)
    {
        ReportError("sendto client failed");
    }
    
#if SERVER_VERBOSE
    printf(">> Processed: %s (using Remote DNS Server) %ld ms\n", packet.mQuestionName.c_str(), elapsedMS);
    fflush(stdout);
#endif
    
#if SERVER_USE_CACHE
    //
    // Add to CacheMap
    //
    pair<unsigned char*, size_t> packetOut;
    packetOut.first = packet.mRawPacketData;
    packetOut.second = packet.mRawPacketLen;
    mServer->AddToCacheMap(thisReq->mDomainName, packetOut);
#endif
    
    return 0;
}


//################################################################################
//##
//## Class: ServerThreadMaintainence
//##
//##  Desc: Thread that actively times out failed requests.
//##
//################################################################################


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ServerThreadMaintainence::ThreadMain()
//  Description: Main thread entry point.
//
//////////////////////////////////////////////////////////////////////////////////

void ServerThreadMaintainence::ThreadMain()
{
    //
    // Actively time out Requests every X milliseconds. Right now this is the only
    // task in our maintainence thread, but this could be expanded as needs arise.
    //
    while (!mServer->ShuttingDown())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(SERVER_TIMEOUT_SCAN_MS));
        mServer->OutboxTimeout();
    }
}

