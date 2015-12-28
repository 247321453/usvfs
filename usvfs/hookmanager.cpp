/*
Userspace Virtual Filesystem

Copyright (C) 2015 Sebastian Herbord. All rights reserved.

This file is part of usvfs.

usvfs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

usvfs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with usvfs. If not, see <http://www.gnu.org/licenses/>.
*/
#include "hookmanager.h"
#include "hooks/ntdll.h"
#include "hooks/kernel32.h"
#include "hooks/ole32.h"
#include "exceptionex.h"
#include "usvfs.h"
#include <utility.h>
#include <stdexcept>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <shmlogger.h>
#include <logging.h>
#include <directory_tree.h>
#include <usvfsparameters.h>
#include <winapi.h>


using namespace HookLib;
namespace uhooks = usvfs::hooks;
namespace bf = boost::filesystem;


namespace usvfs {


HookManager *HookManager::s_Instance = nullptr;


HookManager::HookManager(const Parameters &params, HMODULE module)
  : m_Context(params, module)
{
  if (s_Instance != nullptr) {
    throw std::runtime_error("singleton duplicate instantiation (HookManager)");
  }

  s_Instance = this;

  m_Context.registerProcess(::GetCurrentProcessId());

  winapi::ex::OSVersion version = winapi::ex::getOSVersion();
  spdlog::get("usvfs")->info("Windows version {}.{} sp {}",
                             version.major, version.minor, version.servicpack);

  initHooks();

  if (params.debugMode) {
    MessageBoxA(nullptr, "Hooks initialized", "Pause", MB_OK);
  }
}

HookManager::~HookManager()
{
  spdlog::get("hooks")->debug("end hook of process {}", GetCurrentProcessId());
  removeHooks();
  m_Context.unregisterCurrentProcess();
}

HookManager &HookManager::instance()
{
  if (s_Instance == nullptr) {
    throw std::runtime_error("singleton not instantiated");
  }

  return *s_Instance;
}

LPVOID HookManager::detour(const char *functionName)
{
  auto iter = m_Hooks.find(functionName);
  if (iter != m_Hooks.end()) {
    return GetDetour(iter->second);
  } else {
    return nullptr;
  }
}

void HookManager::removeHook(const std::string &functionName)
{
  auto iter = m_Hooks.find(functionName);
  if (iter != m_Hooks.end()) {
    RemoveHook(iter->second);
    m_Hooks.erase(iter);
    spdlog::get("usvfs")->info("removed hook for {}", functionName);
  } else {
    spdlog::get("usvfs")->info("{} wasn't hooked", functionName);
  }
}

void HookManager::logStubInt(LPVOID address)
{
  if (m_Stubs.find(address) != m_Stubs.end()) {
    spdlog::get("hooks")->debug("{0} called", m_Stubs[address]);
  } else {
    spdlog::get("hooks")->debug("unknown function at {0} called", address);
  }
}

void HookManager::logStub(LPVOID address)
{
  try {
    instance().logStubInt(address);
  } catch (const std::exception &e) {
    spdlog::get("hooks")->debug("function at {0} called after shutdown: {1}", address, e.what());
  }
}

void HookManager::installHook(HMODULE module1, HMODULE module2, const std::string &functionName, LPVOID hook)
{
  BOOST_ASSERT(hook != nullptr);
  HOOKHANDLE handle = INVALID_HOOK;
  HookError err = ERR_NONE;
  LPVOID funcAddr = nullptr;
  HMODULE usedModule = nullptr;
  // both module1 and module2 are allowed to be null
  if (module1 != nullptr) {
    funcAddr = MyGetProcAddress(module1, functionName.c_str());
    if (funcAddr != nullptr) {
      handle = InstallHook(funcAddr, hook, &err);
    }
    if (handle != INVALID_HOOK) usedModule = module1;
  }

  if ((handle == INVALID_HOOK) && (module2 != nullptr)) {
    funcAddr = MyGetProcAddress(module2, functionName.c_str());
    if (funcAddr != nullptr) {
      handle = InstallHook(funcAddr, hook, &err);
    }
    if (handle != INVALID_HOOK) usedModule = module2;
  }

  if (handle == INVALID_HOOK) {
    spdlog::get("usvfs")->error("failed to hook {0}: {1}",
      functionName, GetErrorString(err));
  } else {
    m_Stubs.insert(make_pair(funcAddr, functionName));
    m_Hooks.insert(make_pair(std::string(functionName), handle));
    spdlog::get("usvfs")->info("hooked {0} in {1} type {2}",
      functionName, winapi::ansi::getModuleFileName(usedModule), GetHookType(handle));
  }

}

void HookManager::installStub(HMODULE module1, HMODULE module2, const std::string &functionName)
{
  HOOKHANDLE handle = INVALID_HOOK;
  HookError err = ERR_NONE;
  LPVOID funcAddr = nullptr;
  HMODULE usedModule = nullptr;
  // both module1 and module2 are allowed to be null
  if (module1 != nullptr) {
    funcAddr = MyGetProcAddress(module1, functionName.c_str());
    if (funcAddr != nullptr) {
      handle = InstallStub(funcAddr, logStub, &err);
    }
    if (handle != INVALID_HOOK) usedModule = module1;
  }

  if ((handle == INVALID_HOOK) && (module2 != nullptr)) {
    funcAddr = MyGetProcAddress(module2, functionName.c_str());
    if (funcAddr != nullptr) {
      handle = InstallStub(funcAddr, logStub, &err);
    }
    if (handle != INVALID_HOOK) usedModule = module2;
  }

  if (handle == INVALID_HOOK) {
    spdlog::get("usvfs")->error("failed to stub {0}: {1}", functionName, GetErrorString(err));
  } else {
    m_Stubs.insert(make_pair(funcAddr, functionName));
    m_Hooks.insert(make_pair(std::string(functionName), handle));
    spdlog::get("usvfs")->info("stubbed {0} in {1} type {2}",
      functionName, winapi::ansi::getModuleFileName(usedModule), GetHookType(handle));
  }
}

void HookManager::initHooks()
{
  HMODULE k32Mod = GetModuleHandleA("kernel32.dll");
  spdlog::get("usvfs")->debug("kernel32.dll at {0:x}", reinterpret_cast<unsigned long>(k32Mod));
  // kernelbase.dll contains the actual implementation for functions formerly in
  // kernel32.dll and advapi32.dll, starting with Windows 7
  // http://msdn.microsoft.com/en-us/library/windows/desktop/dd371752(v=vs.85).aspx
  HMODULE kbaseMod = GetModuleHandleA("kernelbase.dll");
  spdlog::get("usvfs")->debug("kernelbase.dll at {0:x}", reinterpret_cast<unsigned long>(kbaseMod));

  installHook(kbaseMod, k32Mod, "GetFileAttributesExW", uhooks::GetFileAttributesExW);
//  installHook(kbaseMod, k32Mod, "GetFileAttributesW", uhooks::GetFileAttributesW);
  installHook(kbaseMod, k32Mod, "SetFileAttributesW", uhooks::SetFileAttributesW);
  installHook(kbaseMod, k32Mod, "CreateFileW", uhooks::CreateFileW); // not all calls seem to translate to a call to NtCreateFile
  installStub(kbaseMod, k32Mod, "CreateFileExW");
//  installStub(kbaseMod, k32Mod, "CreateDirectoryW");
  installStub(kbaseMod, k32Mod, "DeleteFileW");
  installStub(kbaseMod, k32Mod, "DeleteFileA");
  installHook(kbaseMod, k32Mod, "GetCurrentDirectoryW", uhooks::GetCurrentDirectoryW);
  installHook(kbaseMod, k32Mod, "SetCurrentDirectoryW", uhooks::SetCurrentDirectoryW);

  //installHook(kbaseMod, k32Mod, "ExitProcess", uhooks::ExitProcess);

  installHook(kbaseMod, k32Mod, "CreateProcessA", uhooks::CreateProcessA);
  installHook(kbaseMod, k32Mod, "CreateProcessW", uhooks::CreateProcessW);
  installStub(kbaseMod, k32Mod, "CreateJobObjectA");
  installStub(kbaseMod, k32Mod, "CreateJobObjectW");

  installStub(kbaseMod, k32Mod, "MoveFileA");
  installStub(kbaseMod, k32Mod, "MoveFileExA");
  installStub(kbaseMod, k32Mod, "MoveFileW");
  installHook(kbaseMod, k32Mod, "MoveFileExW", uhooks::MoveFileExW);
/*
  installStub(kbaseMod, k32Mod, "GetPrivateProfileStringA");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileStringW");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileStructA");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileStructW");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileSectionNamesA");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileSectionNamesW");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileSectionA");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileSectionW");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileIntA");
  installStub(kbaseMod, k32Mod, "GetPrivateProfileIntW");

  installStub(kbaseMod, k32Mod, "WritePrivateProfileSectionA");
  installStub(kbaseMod, k32Mod, "WritePrivateProfileSectionW");
  installStub(kbaseMod, k32Mod, "WritePrivateProfileStringA");
  installStub(kbaseMod, k32Mod, "WritePrivateProfileStringW");
  installStub(kbaseMod, k32Mod, "WritePrivateProfileStructA");
  installStub(kbaseMod, k32Mod, "WritePrivateProfileStructW");
*/
  installStub(kbaseMod, k32Mod, "CopyFileA");
  installStub(kbaseMod, k32Mod, "CopyFileW");
  installStub(kbaseMod, k32Mod, "CreateHardLinkA");
  installStub(kbaseMod, k32Mod, "CreateHardLinkW");
  installHook(kbaseMod, k32Mod, "GetFullPathNameW", uhooks::GetFullPathNameW);

  HMODULE ntdllMod = GetModuleHandleA("ntdll.dll");
  spdlog::get("usvfs")->debug("ntdll.dll at {0:x}", reinterpret_cast<unsigned long>(ntdllMod));
  installHook(ntdllMod, nullptr, "NtQueryFullAttributesFile", uhooks::NtQueryFullAttributesFile);
  installHook(ntdllMod, nullptr, "NtQueryAttributesFile", uhooks::NtQueryAttributesFile);
  installHook(ntdllMod, nullptr, "NtQueryDirectoryFile", uhooks::NtQueryDirectoryFile);
  installHook(ntdllMod, nullptr, "NtOpenFile", uhooks::NtOpenFile);
  installHook(ntdllMod, nullptr, "NtCreateFile", uhooks::NtCreateFile);
  installHook(ntdllMod, nullptr, "NtClose", uhooks::NtClose);
  installStub(ntdllMod, nullptr, "NtDeleteFile");

  HMODULE shellMod = GetModuleHandleA("shell32.dll");
  if (shellMod != nullptr) {
    spdlog::get("usvfs")->debug("shell32.dll at {0:x}", reinterpret_cast<unsigned long>(shellMod));
    installStub(shellMod, nullptr, "SHFileOperationA");
    installStub(shellMod, nullptr, "SHFileOperationW");
    installStub(shellMod, nullptr, "ShellExecuteA");
    installStub(shellMod, nullptr, "ShellExecuteW");
    installStub(shellMod, nullptr, "ShellExecuteExA");
    installStub(shellMod, nullptr, "ShellExecuteExW");
  }

  HMODULE versionMod = GetModuleHandleA("version.dll");
  if (versionMod != nullptr) {
    spdlog::get("usvfs")->debug("version.dll at {0:x}", reinterpret_cast<unsigned long>(versionMod));
    installStub(versionMod, nullptr, "GetFileVersionInfoW");
    installStub(versionMod, nullptr, "GetFileVersionInfoSizeW");
  }

/*  HMODULE oleMod = GetModuleHandleA("ole32.dll");
  if (oleMod != nullptr) {
    spdlog::get("usvfs")->debug("ole32.dll at {0:x}", reinterpret_cast<unsigned long>(oleMod));
    installHook(oleMod, nullptr, "CoCreateInstance", uhooks::CoCreateInstance);
    installHook(oleMod, nullptr, "CoCreateInstanceEx", uhooks::CoCreateInstanceEx);
  }
*/
  installHook(kbaseMod, k32Mod, "LoadLibraryExW", uhooks::LoadLibraryExW);
  installHook(kbaseMod, k32Mod, "LoadLibraryExA", uhooks::LoadLibraryExA);
  installHook(kbaseMod, k32Mod, "LoadLibraryW", uhooks::LoadLibraryW);
  installHook(kbaseMod, k32Mod, "LoadLibraryA", uhooks::LoadLibraryA);

  // install this hook late as usvfs is calling it itself for debugging purposes
//  installHook(k32Mod, kbaseMod, "GetModuleFileNameW", uhooks::GetModuleFileNameW);

  spdlog::get("usvfs")->debug("hooks installed");
}


void HookManager::removeHooks()
{
  while (m_Hooks.size() > 0) {
    auto iter = m_Hooks.begin();
    spdlog::get("usvfs")->debug("remove hook {}", iter->first);
    RemoveHook(iter->second);
    m_Hooks.erase(iter);
  }
}

} // namespace usvfs