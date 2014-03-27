//////////////////////////////////////////////////////////////////////////////////
//
// File: Error.cpp
//
// Desc:
//
//////////////////////////////////////////////////////////////////////////////////
#include <stdlib.h>
#include <iostream>
#include "Error.h"

using namespace std;

//////////////////////////////////////////////////////////////////////////////////
//
//     Function: ReportErrorMain
//  Description: Main error handling function. Currently it just writes to
//               stderr on the console but it could be expanded to log these
//               errors to a file with a timestamp, etc.
//       Inputs: format + arguments: just like sprintf, printf, etc.
//        Notes: Use the ReportError() macro to invoke this.
//
//////////////////////////////////////////////////////////////////////////////////

void ReportErrorMain(
                     const char *inFile, int    inLine, const char *inFunc,
                     const char *inFormat, ...)
{
    // First append "FILE:LINENO:FUNC: " to the buffer
    char buf[2048], *bufPtr = buf;
    bufPtr += sprintf(buf, "%s:%d:%s: ", inFile, inLine, inFunc);
    
    // Now add formated text, append a newline
    va_list ap;
    va_start(ap, inFormat);
    bufPtr += vsprintf(bufPtr, inFormat, ap);
    va_end(ap);
    strcpy(bufPtr, "\n");
    
    // Console it
    cerr << buf;
    fflush(stderr);
}
