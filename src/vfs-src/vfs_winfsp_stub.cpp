/*
 * vfs_winfsp_stub.cpp - Stub implementation when WinFsp SDK is not available
 *
 * Compile: g++ -c vfs_winfsp_stub.cpp -o vfs_winfsp_stub.o -std=c++17
 *
 * Provides null implementations of WinFsp platform functions.
 */

#include "vfs_platform.hpp"

IVfsPlatform* createWinFspPlatform() {
    return nullptr;
}

IVfsPlatform* createFusePlatform() {
    return nullptr;
}
