//////////////////////////////////////////////////////////////////////////////////
//
// Notes for the code viewer
//
//////////////////////////////////////////////////////////////////////////////////
//
// Usage:
//    ./simpleServerDNS [<listenPort> <remoteDNSAddr> <remoteDNSPort>]
//
//    listenPort: is what our server listens on. (default: 53)
//    remoteDNSAddr: Where to forward DNS requests (default: 8.8.8.8)
//    remoteDNSPort: Where to forward DNS requests (default: 53)
//
//
//////////////////////////////////////////////////////////////////////////////////
//
// Approach:
//
//    4 threads handle the server. Summary of the thread processing loops below.
//
//    Inbox thread:
//        - Reads packets on port 53 (blocking) [Socket #1]
//        - Adds them to the processing queue as a request object
//    Processing thread:
//        - Pops request objects off the processing queue
//        - Decodes and verifies raw packet data in the request object
//        - (Optionally) checks cache and responds with cache hit [Done.]
//        - Replaces the packet ID with our own ID
//        - Sends packet to remote DNS server [Socket #2]
//        - Adds request to the outbox
// Outbox thread:
//        - Reads packet responses from remote DNS server [Socket #2]
//        - Matches remote DNS server response with request object in the outbox
//        - Removes request object from the outbox
//        - Restores original packet ID to the response packet
//        - Checks timeout threshold before sending
//        - Sends response packet to the original requestee [Socket #1]
// Maintainence thread:
//        - Runs every X milliseconds
//        - Actively culls timed out request objects from the outbox
//
//////////////////////////////////////////////////////////////////////////////////
//
// Notes:
//
// To gracefully shutdown this server:
//        "kill -s TERM <pid>" or just hit Ctrl+C
//
// To test the server running port 2000:
//         dig @127.0.0.1 -p 2000 google.com
//
// To run this on 53
//        sudo lsof -i :53 | grep LISTEN
//        stop any server on 53 (if any), run this one.
//
//////////////////////////////////////////////////////////////////////////////////

//
// Now onto the code!
//
#include <stdlib.h>
#include <iostream>
#include <cassert>
#include <thread>
#include "Error.h"
#include "Packet.h"
#include "Server.h"

using namespace std;


//################################################################################
//##
//## Program main() and any helper functions
//##
//################################################################################


//////////////////////////////////////////////////////////////////////////////////
//
//     Function: main
//  Description: Main function.
//       Inputs: argc: number of arguments.
//               argv: string argument list.
//      Outputs: Non-zero on error.
//
//////////////////////////////////////////////////////////////////////////////////

int main(int argc, char * const argv[])
{
    int rc;
    
    cout << "\nStarting server...\n";
    unsigned short listenPort = argc > 1 ? atoi(argv[1]) : 53;
    const char *fwdTo = argc > 2 ? argv[2] : "8.8.8.8"; /* Google's public DNS server */
    unsigned short fwdToPort = argc > 3 ? atoi(argv[3]) : 53;
    
    try
    {
        Server dnsServer(listenPort, fwdTo, fwdToPort);
        rc = dnsServer.RunServer();
        if (rc)
        {
            ReportError("Failed to run server with options: %u, %s, %u",
                        listenPort, fwdTo, fwdToPort);
            return -1;
        }
        cout << "Done.\n";
    }
    catch(...)
    {
        ReportError("Exiting, exception caught");
        return -1;
    }
    
    return 0;
}


