//////////////////////////////////////////////////////////////////////////////////
//
// File: Error.h
//
// Desc:
//
//////////////////////////////////////////////////////////////////////////////////
#ifndef ERROR_H
#define ERROR_H

void ReportErrorMain(
                     const char *inFile, int	inLine, const char *inFunc,
                     const char *inFormat, ...);

#define ReportError(ARGS...) \
    ReportErrorMain(__FILE__, __LINE__, __func__, ARGS)


#endif
