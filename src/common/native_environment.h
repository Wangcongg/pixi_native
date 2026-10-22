#pragma once
namespace pixinative_environment {
#if defined(PIXINATIVE_PRODUCTION)
inline constexpr bool kProductionBuild=true;
#else
inline constexpr bool kProductionBuild=false;
#endif
inline constexpr const char* kDefaultGameEnvironment=nullptr;
inline constexpr const char* kDefaultGameMode=nullptr;
inline constexpr const char* kDefaultAppId=nullptr;
inline constexpr const char* kDefaultServerUrl=nullptr;
}
