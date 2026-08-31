// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

// Godot replaces the upstream OCIOZArchive.cpp with this stub. The original
// implementation is the only part of OpenColorIO that depends on minizip-ng,
// and it exists purely to read and write `.ocioz` archived configs. Godot loads
// configs from plain `.ocio` files and from OpenColorIO's built-in configs, so
// the dependency is not worth vendoring. Every entry point below fails loudly
// rather than silently returning wrong colour.
//
// To restore archive support: vendor minizip-ng, drop this file, and take
// OCIOZArchive.cpp from upstream unmodified.

#include "OCIOZArchive.h"
#include "Platform.h"

namespace OCIO_NAMESPACE
{

namespace
{
constexpr char kUnsupported[] =
    "OCIOZ archives are not supported in this build of OpenColorIO. "
    "Use an unpacked .ocio config instead.";
}

void archiveConfig(std::ostream &, const Config &, const char *)
{
    throw Exception(kUnsupported);
}

std::vector<uint8_t> getFileBufferFromArchive(const std::string &, const std::string &)
{
    throw Exception(kUnsupported);
}

std::vector<uint8_t> getFileBufferFromArchiveByExtension(const std::string &, const std::string &)
{
    throw Exception(kUnsupported);
}

void getEntriesMappingFromArchiveFile(const std::string &, std::map<std::string, std::string> &)
{
    throw Exception(kUnsupported);
}

std::vector<uint8_t> CIOPOciozArchive::getLutData(const char *) const
{
    throw Exception(kUnsupported);
}

std::string CIOPOciozArchive::getConfigData() const
{
    throw Exception(kUnsupported);
}

std::string CIOPOciozArchive::getFastLutFileHash(const char *) const
{
    throw Exception(kUnsupported);
}

void CIOPOciozArchive::setArchiveAbsPath(const std::string & absPath)
{
    m_archiveAbsPath = absPath;
}

void CIOPOciozArchive::buildEntries()
{
    throw Exception(kUnsupported);
}

} // namespace OCIO_NAMESPACE
