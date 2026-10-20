#pragma once

#if defined(_WIN32)
#if defined(TCP_HELPER_BUILDING_DLL)
#define TCP_HELPER_API __declspec(dllexport)
#else
#define TCP_HELPER_API __declspec(dllimport)
#endif
#else
#define TCP_HELPER_API __attribute__((visibility("default")))
#endif
