#ifndef COREUTIL_H_
#define COREUTIL_H_

#include <QString>

#include "mythexp.h"

MPUBLIC long long getDiskSpace(const QString&,long long&,long long&);

MPUBLIC bool extractZIP(const QString &zipFile, const QString &outDir);

#endif // COREUTIL_H_
