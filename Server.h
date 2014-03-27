//////////////////////////////////////////////////////////////////////////////////
//
// File: Server.h
//
// Desc: Server and ServerThread classes.
//
//////////////////////////////////////////////////////////////////////////////////
#ifndef SERVER_H
#define SERVER_H
#include <netinet/in.h>
#include <semaphore.h>
#include <thread>
#include <string>
#include <queue>
#include <list>
#include <array>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

using namespace std;

//################################################################################
//##
//## Class: Server
//##
//##  Desc: Main server representation object.
//##
//################################################################################
#define SERVER_BUFFER_SIZE       4096
#define SERVER_MAX_PACKET_SIZE   512         /* Accept no packets over this size */
#define SERVER_TIMEOUT_MS        2000        /* How long till a request times out */
#define SERVER_TIMEOUT_SCAN_MS   1000        /* How often to we scan for timeouts */
#define SERVER_VERBOSE           1           /* On/off: Live processing output */
#define SERVER_USE_CACHE         0           /* On/off: Use a simple cache */

class ServerInbox;
class Request;
class DNSPacket;
class ServerThreadInbox;
class ServerThreadProcess;
class ServerThreadOutbox;
class ServerThreadMaintainence;

class Server
{
    // Allows access to the atomic ints for statistics
public:
    //
    // Constructors/Destructors
    //
    Server(unsigned short inListenPort, const char* inFwdStr,
           unsigned short inFwdPort);
    virtual ~Server();
    
    //
    // Get/set member functions
    //
    int GetServerSocket() { return mServerSocket; }
    int GetFwdSocket() { return mFwdSocket; }
    const struct sockaddr_in* GetFwdSocketAddr() { return &mFwdSocketAddr; }
    
    //
    // Public member functions
    //
    int                            RunServer();
    bool                           ShuttingDown() { return mShuttingDown; }
    static void                    HandleSignal(int inSig);
    int                            InboxQueueWaitForData();
    int                            InboxQueuePushBack(unique_ptr<Request> inReq);
    unique_ptr<Request>            InboxQueuePopFront();
    unsigned short                 GenerateUniqueID();
    int                            OutboxWaitForData();
    int                            OutboxAdd(unique_ptr<Request> inReq);
    unique_ptr<Request>            OutboxRemove(unsigned short inID);
    void                           OutboxTimeout();
#if SERVER_USE_CACHE
    int                            AddToCacheMap(string inDomain, pair<unsigned char*, size_t>& inPacket);
    bool                           CheckCacheMap(string inDomain, pair<unsigned char*, size_t>& outPacket);
#endif
    
    //
    // Public data
    //
    atomic_int                     mStatsPacketsIn;
    atomic_int                     mStatsPacketsOut;
    atomic_int                     mStatsRequests;
    atomic_int                     mStatsServed;
    atomic_int                     mStatsTimeOuts;
    
    //
    // Protected data
    //
protected:
    bool                           mShuttingDown;
    list<ServerThreadInbox*>       mInboxThreads;
    list<ServerThreadProcess*>     mProcessThreads;
    list<ServerThreadOutbox*>      mOutboxThreads;
    ServerThreadMaintainence*      mMaintainenceThread;
    static condition_variable      sShuttingDownCV;
    static mutex                   sShuttingDownCVMutex;
    
    // Network Data: Local Server
    unsigned short                 mServerPort;
    int                            mServerSocket;
    struct sockaddr_in             mServerSocketAddr;
    
    // Network Data: Remote/Forward DNS Server
    string                         mFwdStr;
    unsigned short                 mFwdPort;
    int                            mFwdSocket;
    struct sockaddr_in             mFwdSocketAddr;
    
    // Unique Packet ID Generator
    unsigned short                 mGenIDCounter;
    recursive_mutex                mGenIDMutex;
    
    // InboxQueue (Inbox Thread)
    queue<unique_ptr<Request>>     mInboxQueue;
    recursive_mutex                mInboxQueueMutex;
    sem_t*                         mInboxQueueSemaphore;
    
    // OutboxQueue (Outbox Thread)
    array<unique_ptr<Request>, USHRT_MAX> mOutboxArray; // Used for: Successful replies
    queue<unsigned short>          mOutboxQueue; // Used for: Active timeouts
    recursive_mutex                mOutboxMutex;
    sem_t*                         mOutboxSemaphore;
    
#if SERVER_USE_CACHE
    // Simple caching mechanism for testing (Process Thread)
    unordered_map<string,pair<unsigned char*, size_t>> mCacheMap;
    recursive_mutex                mCacheMapMutex;
#endif
};


//################################################################################
//##
//## Class: ServerThread
//##
//##  Desc: Server thread base class.
//##
//################################################################################

class ServerThread
{
public:
    //
    // Constructors/Destructors
    //
    ServerThread(Server *inServer) : mServer(inServer), mThread(nullptr) { }
    virtual ~ServerThread() { if (mThread) delete mThread; }
    
    //
    // Public member functions
    //
    virtual void ThreadMain() = 0;
    virtual void SetThread(thread *inThread) { mThread = inThread; }
    virtual thread* GetThread() { return mThread; }
    
    //
    // Protected data
    //
protected:
    Server    *mServer;
    thread    *mThread;
};


//################################################################################
//##
//## Class: ServerThreadInbox
//##
//##  Desc: Reads packets off InboxPort (53 generally) and adds them to the
//##        the inbox queue.
//##
//################################################################################

class ServerThreadInbox : public ServerThread
{
public:
    //
    // Constructors/Destructors
    //
    ServerThreadInbox(Server *inServer) : ServerThread(inServer) { }
    virtual ~ServerThreadInbox() { }
    
    //
    // Public member functions
    //
    virtual void ThreadMain();
    
    //
    // Protected member functions
    //
protected:
    int HandlePacket(unsigned char *inData, size_t inLen, struct sockaddr_in *inFrom);
};


//################################################################################
//##
//## Class: ServerThreadProcess
//##
//##  Desc: Pops Request packets off the inbox queue and processes them. They are
//##        handled (caching) or the packet is forwarded to the remote/forward
//##        DNS server and this Request is moved into the Outbox.
//##
//################################################################################

class ServerThreadProcess : public ServerThread
{
public:
    //
    // Constructors/Destructors
    //
    ServerThreadProcess(Server *inServer) : ServerThread(inServer) { }
    virtual ~ServerThreadProcess() { }
    
    //
    // Public member functions
    //
    virtual void ThreadMain();
    
    //
    // Protected member functions
    //
protected:
    int HandleRequest(unique_ptr<Request> inReq);
};


//################################################################################
//##
//## Class: ServerThreadOutbox
//##
//##  Desc: Waits for replies from the remote/forward DNS server. When received
//##        it sends the reply to the original client. It also handles timeouts.
//##
//################################################################################

class ServerThreadOutbox : public ServerThread
{
public:
    //
    // Constructors/Destructors
    //
    ServerThreadOutbox(Server *inServer) : ServerThread(inServer) { }
    virtual ~ServerThreadOutbox() { }
    
    //
    // Public member functions
    //
    virtual void ThreadMain();
    
    //
    // Protected member functions
    //
protected:
    int HandlePacket(unsigned char *inData, size_t inLen, struct sockaddr_in *inFrom);
};


//################################################################################
//##
//## Class: ServerThreadMaintainence
//##
//##  Desc: Thread that actively times out failed requests.
//##
//################################################################################

class ServerThreadMaintainence : public ServerThread
{
public:
    //
    // Constructors/Destructors
    //
    ServerThreadMaintainence(Server *inServer) : ServerThread(inServer) { }
    virtual ~ServerThreadMaintainence() { }
    
    //
    // Public member functions
    //
    virtual void ThreadMain();
};


#endif

