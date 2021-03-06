// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cstddef>
#include <cstdlib>
#include <set>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MemArena.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef ANDROID
#include <linux/ashmem.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach_init.h>
#include <mach/vm_map.h>
#endif
#endif

#ifdef ANDROID
#define ASHMEM_DEVICE "/dev/ashmem"

static int AshmemCreateFileMapping(const char* name, size_t size)
{
  int fd, ret;
  fd = open(ASHMEM_DEVICE, O_RDWR);
  if (fd < 0)
    return fd;

  // We don't really care if we can't set the name, it is optional
  ioctl(fd, ASHMEM_SET_NAME, name);

  ret = ioctl(fd, ASHMEM_SET_SIZE, size);
  if (ret < 0)
  {
    close(fd);
    NOTICE_LOG(MEMMAP, "Ashmem returned error: 0x%08x", ret);
    return ret;
  }
  return fd;
}
#endif

void MemArena::GrabSHMSegment(size_t size)
{
#ifdef _WIN32
  hMemoryMapping =
      CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)(size), nullptr);
#elif defined(ANDROID)
  fd = AshmemCreateFileMapping("Dolphin-emu", size);
  if (fd < 0)
  {
    NOTICE_LOG(MEMMAP, "Ashmem allocation failed");
    return;
  }
#elif defined(__APPLE__)
  machSize = size;

  kern_return_t err = vm_allocate(mach_task_self(), &machAddress, machSize, VM_FLAGS_ANYWHERE);
  if (err != KERN_SUCCESS)
  {
    NOTICE_LOG(MEMMAP, "Mach allocation failed");
    return;
  }
#else
  for (int i = 0; i < 10000; i++)
  {
    std::string file_name = StringFromFormat("/dolphinmem.%d", i);
    fd = shm_open(file_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd != -1)
    {
      shm_unlink(file_name.c_str());
      break;
    }
    else if (errno != EEXIST)
    {
      ERROR_LOG(MEMMAP, "shm_open failed: %s", strerror(errno));
      return;
    }
  }
  if (ftruncate(fd, size) < 0)
    ERROR_LOG(MEMMAP, "Failed to allocate low memory space");
#endif
}

void MemArena::ReleaseSHMSegment()
{
#ifdef _WIN32
  CloseHandle(hMemoryMapping);
  hMemoryMapping = 0;
#elif defined(__APPLE__)
  kern_return_t err = vm_deallocate(mach_task_self(), machAddress, machSize);
  if (err != KERN_SUCCESS)
  {
    NOTICE_LOG(MEMMAP, "Mach deallocation failed");
    return;
  }
#else
  close(fd);
#endif
}

void* MemArena::CreateView(s64 offset, size_t size, void* base)
{
#ifdef _WIN32
  return MapViewOfFileEx(hMemoryMapping, FILE_MAP_ALL_ACCESS, 0, (DWORD)((u64)offset), size, base);
#elif defined(__APPLE__)
  vm_prot_t curProtection;
  vm_prot_t maxProtection;
  kern_return_t err = vm_remap(mach_task_self(), (vm_address_t*)&base, (vm_size_t)size, 0,
                               VM_FLAGS_FIXED, mach_task_self(), machAddress + (vm_address_t)offset,
                               0, &curProtection, &maxProtection, VM_INHERIT_COPY);
  if (err != KERN_SUCCESS)
  {
    NOTICE_LOG(MEMMAP, "vm_remap failed");
    return nullptr;
  }

  return base;
#else
  void* retval = mmap(base, size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | ((base == nullptr) ? 0 : MAP_FIXED), fd, offset);

  if (retval == MAP_FAILED)
  {
    NOTICE_LOG(MEMMAP, "mmap failed");
    return nullptr;
  }
  else
  {
    return retval;
  }
#endif
}

void MemArena::ReleaseView(void* view, size_t size)
{
#ifdef _WIN32
  UnmapViewOfFile(view);
#elif defined(__APPLE__)
  vm_deallocate(mach_task_self(), (vm_address_t)view, (vm_size_t)size);
#else
  munmap(view, size);
#endif
}

u8* MemArena::FindMemoryBase()
{
#ifdef _WIN32
  // 64 bit
  u8* base = (u8*)VirtualAlloc(0, 0x400000000, MEM_RESERVE, PAGE_READWRITE);
  VirtualFree(base, 0, MEM_RELEASE);
  return base;
#elif defined(__APPLE__)
#if _ARCH_64 && !(TARGET_OS_IPHONE && defined(__arm64__))
  vm_size_t size = 0x400000000;
#else
  // iOS is 64-bit, but has a tiny address space, so match 32-bit.
  vm_size_t size = 0x31000000;
#endif
  vm_address_t address;

  kern_return_t err = vm_allocate(mach_task_self(), &address, size, VM_FLAGS_ANYWHERE);
  if (err != KERN_SUCCESS)
  {
    return nullptr;
  }

  err = vm_deallocate(mach_task_self(), address, size);
  if (err != KERN_SUCCESS)
  {
    return nullptr;
  }

  return (u8*)address;
#elif _ARCH_64
  // Very precarious - mmap cannot return an error when trying to map already used pages.
  // This makes the Windows approach above unusable on Linux, so we will simply pray...
  return reinterpret_cast<u8*>(0x2300000000ULL);
#else  // 32 bit
#ifdef ANDROID
  // Android 4.3 changed how mmap works.
  // if we map it private and then munmap it, we can't use the base returned.
  // This may be due to changes in them support a full SELinux implementation.
  const int flags = MAP_ANON | MAP_SHARED;
#else
  const int flags = MAP_ANON | MAP_PRIVATE;
#endif
  const u32 MemSize = 0x31000000;
  void* base = mmap(0, MemSize, PROT_NONE, flags, -1, 0);
  if (base == MAP_FAILED)
  {
    PanicAlert("Failed to map 1 GB of memory space: %s", strerror(errno));
    return 0;
  }
  munmap(base, MemSize);
  return static_cast<u8*>(base);
#endif
}
