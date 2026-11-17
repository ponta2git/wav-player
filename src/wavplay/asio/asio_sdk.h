#pragma once

// The only include point for the Steinberg ASIO SDK. iasiodrv.h has no include
// guard, so including it through this header keeps IASIO defined exactly once
// per translation unit, in the order the SDK expects.

#include <windows.h>
#include <unknwn.h>

#include <asiosys.h>
#include <asio.h>
#include <iasiodrv.h>

#include <cstddef>
#include <type_traits>

// These structures cross the driver DLL boundary. ASIO's Windows interface uses
// 4-byte packing even though its buffer addresses are 64-bit pointers.
static_assert(sizeof(void*) == 8, "WAV Player hosts x64 ASIO drivers only");
static_assert(std::is_base_of_v<IUnknown, IASIO>);
static_assert(std::is_same_v<ASIOSampleRate, double> && std::is_same_v<ASIOSampleType, long>);
static_assert(sizeof(ASIOSamples) == 8 && offsetof(ASIOSamples, lo) == 4);
static_assert(alignof(ASIOBufferInfo) == 4 && sizeof(ASIOBufferInfo) == 24);
static_assert(offsetof(ASIOBufferInfo, buffers) == 8);
