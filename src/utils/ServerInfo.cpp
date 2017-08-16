#include "ServerInfo.h"

namespace Interceptor::ServerInfo {

  std::string commonName()
  {
    return name() + "/" + version() + " (" + osName() + ")";
  }

  std::string version()
  {
    return SERVER_VERSION;
  }

  std::string name()
  {
    return SERVER_NAME;
  }

  std::string osName()
  {
#ifdef _WIN32
    return "Windows 32-bit";
#elif _WIN64
    return "Windows 64-bit";
#elif __unix || __unix__
    return "Unix";
#elif __APPLE__ || __MACH__
    return "Mac OSX";
#elif __linux__
    return "Linux";
#elif __FreeBSD__
    return "FreeBSD";
#else
    return "Other";
#endif
  }

  std::string build()
  {
    return INTERCEPTOR_GIT_COMMIT_ID;
  }

}