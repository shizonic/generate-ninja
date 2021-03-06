#!/usr/bin/env python
# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from __future__ import print_function
import glob
import json
import os
import pipes
import platform
import re
import shutil
import stat
import subprocess
import sys


script_dir = os.path.dirname(os.path.realpath(__file__))
SRC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Use MSVS2017 as the default toolchain.
CURRENT_DEFAULT_TOOLCHAIN_VERSION = '2017'


def SetEnvironmentAndGetRuntimeDllDirs():
  """Sets up os.environ to use the depot_tools VS toolchain with gyp, and
  returns the location of the VS runtime DLLs so they can be copied into
  the output directory after gyp generation.

  Return value is [x64path, x86path] or None
  """
  vs_runtime_dll_dirs = None
  depot_tools_win_toolchain = 0
  if sys.platform == 'win32' and not depot_tools_win_toolchain:
    if not 'VISUAL_STUDIO_PATH' in os.environ:
      os.environ['VISUAL_STUDIO_PATH'] = DetectVisualStudioPath()
    if not 'VISUAL_STUDIO_VERSION' in os.environ:
      os.environ['VISUAL_STUDIO_VERSION'] = GetVisualStudioVersion()

    # When using an installed toolchain these files aren't needed in the output
    # directory in order to run binaries locally, but they are needed in order
    # to create isolates or the mini_installer. Copying them to the output
    # directory ensures that they are available when needed.
    bitness = platform.architecture()[0]
    # When running 64-bit python the x64 DLLs will be in System32
    x64_path = 'System32' if bitness == '64bit' else 'Sysnative'
    x64_path = os.path.join(r'C:\Windows', x64_path)
    vs_runtime_dll_dirs = [x64_path, r'C:\Windows\SysWOW64']

  return vs_runtime_dll_dirs


def _RegistryGetValueUsingWinReg(key, value):
  """Use the _winreg module to obtain the value of a registry key.

  Args:
    key: The registry key.
    value: The particular registry value to read.
  Return:
    contents of the registry key's value, or None on failure.  Throws
    ImportError if _winreg is unavailable.
  """
  import _winreg
  try:
    root, subkey = key.split('\\', 1)
    assert root == 'HKLM'  # Only need HKLM for now.
    with _winreg.OpenKey(_winreg.HKEY_LOCAL_MACHINE, subkey) as hkey:
      return _winreg.QueryValueEx(hkey, value)[0]
  except WindowsError:
    return None


def _RegistryGetValue(key, value):
  try:
    return _RegistryGetValueUsingWinReg(key, value)
  except ImportError:
    raise Exception('The python library _winreg not found.')


def GetVisualStudioVersion():
  """Return VISUAL_STUDIO_VERSION of Visual Studio.
  """
  return os.environ.get('VISUAL_STUDIO_VERSION', CURRENT_DEFAULT_TOOLCHAIN_VERSION)


def DetectVisualStudioPath():
  """Return path to the VISUAL_STUDIO_VERSION of Visual Studio.
  """

  # Note that this code is used from
  # build/toolchain/win/setup_toolchain.py as well.
  version_as_year = GetVisualStudioVersion()
  year_to_version = {
      '2015': '14.0',
      '2017': '15.0',
  }
  if version_as_year not in year_to_version:
    raise Exception(('Visual Studio version %s (from VISUAL_STUDIO_VERSION)'
                     ' not supported. Supported versions are: %s') % (
                       version_as_year, ', '.join(year_to_version.keys())))
  version = year_to_version[version_as_year]
  if version_as_year == '2017':
    # The VC++ 2017 install location needs to be located using COM instead of
    # the registry. For details see:
    # https://blogs.msdn.microsoft.com/heaths/2016/09/15/changes-to-visual-studio-15-setup/
    # For now we use a hardcoded default with an environment variable override.
    for path in (
        os.environ.get('vs2017_install'),
        os.path.join(os.environ.get('ProgramFiles(x86)'), 'Microsoft Visual Studio', '2017', 'Enterprise'),
        os.path.join(os.environ.get('ProgramFiles(x86)'), 'Microsoft Visual Studio', '2017', 'Professional'),
        os.path.join(os.environ.get('ProgramFiles(x86)'), 'Microsoft Visual Studio', '2017', 'Community')):
      if path and os.path.exists(path):
        return path
  else:
    keys = [r'HKLM\Software\Microsoft\VisualStudio\%s' % version,
            r'HKLM\Software\Wow6432Node\Microsoft\VisualStudio\%s' % version]
    for key in keys:
      path = _RegistryGetValue(key, 'InstallDir')
      if not path:
        continue
      path = os.path.normpath(os.path.join(path, '..', '..'))
      return path

  raise Exception(('Visual Studio Version %s (from VISUAL_STUDIO_VERSION)'
                   ' not found.') % (version_as_year))


def _CopyRuntimeImpl(target, source, verbose=True):
  """Copy |source| to |target| if it doesn't already exist or if it needs to be
  updated (comparing last modified time as an approximate float match as for
  some reason the values tend to differ by ~1e-07 despite being copies of the
  same file... https://crbug.com/603603).
  """
  if (os.path.isdir(os.path.dirname(target)) and
      (not os.path.isfile(target) or
       abs(os.stat(target).st_mtime - os.stat(source).st_mtime) >= 0.01)):
    if verbose:
      print('Copying %s to %s...' % (source, target))
    if os.path.exists(target):
      # Make the file writable so that we can delete it now, and keep it
      # readable.
      os.chmod(target, stat.S_IWRITE | stat.S_IREAD)
      os.unlink(target)
    shutil.copy2(source, target)
    # Make the file writable so that we can overwrite or delete it later,
    # keep it readable.
    os.chmod(target, stat.S_IWRITE | stat.S_IREAD)


def _CopyUCRTRuntime(target_dir, source_dir, target_cpu, dll_pattern, suffix):
  """Copy both the msvcp and vccorlib runtime DLLs, only if the target doesn't
  exist, but the target directory does exist."""
  for file_part in ('msvcp', 'vccorlib', 'vcruntime'):
    dll = dll_pattern % file_part
    target = os.path.join(target_dir, dll)
    source = os.path.join(source_dir, dll)
    _CopyRuntimeImpl(target, source)
  # Copy the UCRT files needed by VS 2015 from the Windows SDK. This location
  # includes the api-ms-win-crt-*.dll files that are not found in the Windows
  # directory. These files are needed for component builds.
  # If WINDOWSSDKDIR is not set use the default SDK path. This will be the case
  # when DEPOT_TOOLS_WIN_TOOLCHAIN=0 and vcvarsall.bat has not been run.
  win_sdk_dir = os.path.normpath(
      os.environ.get('WINDOWSSDKDIR',
                     os.path.join(os.environ.get('ProgramFiles(x86)'),
                                  'Windows Kits', '10')))
  ucrt_dll_dirs = os.path.join(win_sdk_dir, 'Redist', 'ucrt', 'DLLs',
                               target_cpu)
  ucrt_files = glob.glob(os.path.join(ucrt_dll_dirs, 'api-ms-win-*.dll'))
  assert len(ucrt_files) > 0
  for ucrt_src_file in ucrt_files:
    file_part = os.path.basename(ucrt_src_file)
    ucrt_dst_file = os.path.join(target_dir, file_part)
    _CopyRuntimeImpl(ucrt_dst_file, ucrt_src_file, False)
  _CopyRuntimeImpl(os.path.join(target_dir, 'ucrtbase' + suffix),
                    os.path.join(source_dir, 'ucrtbase' + suffix))


def FindVCToolsRoot():
  """In VS2017 the PGO runtime dependencies are located in
  {toolchain_root}/VC/Tools/MSVC/{x.y.z}/bin/Host{target_cpu}/{target_cpu}/, the
  {version_number} part is likely to change in case of a minor update of the
  toolchain so we don't hardcode this value here (except for the major number).

  This returns the '{toolchain_root}/VC/Tools/MSVC/{x.y.z}/bin/' path.

  This function should only be called when using VS2017.
  """
  assert GetVisualStudioVersion() == '2017'
  SetEnvironmentAndGetRuntimeDllDirs()
  assert ('VISUAL_STUDIO_PATH' in os.environ)
  vc_tools_msvc_root = os.path.join(os.environ['VISUAL_STUDIO_PATH'],
      'VC', 'Tools', 'MSVC')
  for directory in os.listdir(vc_tools_msvc_root):
    if not os.path.isdir(os.path.join(vc_tools_msvc_root, directory)):
      continue
    if re.match('14\.\d+\.\d+', directory):
      return os.path.join(vc_tools_msvc_root, directory, 'bin')
  raise Exception('Unable to find the VC tools directory.')


def _CopyPGORuntime(target_dir, target_cpu):
  """Copy the runtime dependencies required during a PGO build.
  """
  env_version = GetVisualStudioVersion()
  # These dependencies will be in a different location depending on the version
  # of the toolchain.
  if env_version == '2015':
    pgo_x86_runtime_dir = os.path.join(os.environ.get('VISUAL_STUDIO_PATH'),
                                       'VC', 'bin')
    pgo_x64_runtime_dir = os.path.join(pgo_x86_runtime_dir, 'amd64')
  elif env_version == '2017':
    pgo_runtime_root = FindVCToolsRoot()
    assert pgo_runtime_root
    # There's no version of pgosweep.exe in HostX64/x86, so we use the copy
    # from HostX86/x86.
    pgo_x86_runtime_dir = os.path.join(pgo_runtime_root, 'HostX86', 'x86')
    pgo_x64_runtime_dir = os.path.join(pgo_runtime_root, 'HostX64', 'x64')
  else:
    raise Exception('Unexpected toolchain version: %s.' % env_version)

  # We need to copy 2 runtime dependencies used during the profiling step:
  #     - pgort140.dll: runtime library required to run the instrumented image.
  #     - pgosweep.exe: executable used to collect the profiling data
  pgo_runtimes = ['pgort140.dll', 'pgosweep.exe']
  for runtime in pgo_runtimes:
    if target_cpu == 'x86':
      source = os.path.join(pgo_x86_runtime_dir, runtime)
    elif target_cpu == 'x64':
      source = os.path.join(pgo_x64_runtime_dir, runtime)
    else:
      raise NotImplementedError("Unexpected target_cpu value: " + target_cpu)
    if not os.path.exists(source):
      raise Exception('Unable to find %s.' % source)
    _CopyRuntimeImpl(os.path.join(target_dir, runtime), source)


def _CopyRuntime(target_dir, source_dir, target_cpu, debug):
  """Copy the VS runtime DLLs, only if the target doesn't exist, but the target
  directory does exist. Handles VS 2015 and VS 2017."""
  suffix = "d.dll" if debug else ".dll"
  # VS 2017 uses the same CRT DLLs as VS 2015.
  _CopyUCRTRuntime(target_dir, source_dir, target_cpu, '%s140' + suffix,
                    suffix)


def CopyDlls(target_dir, configuration, target_cpu):
  """Copy the VS runtime DLLs into the requested directory as needed.

  configuration is one of 'Debug' or 'Release'.
  target_cpu is one of 'x86' or 'x64'.

  The debug configuration gets both the debug and release DLLs; the
  release config only the latter.
  """
  vs_runtime_dll_dirs = SetEnvironmentAndGetRuntimeDllDirs()
  if not vs_runtime_dll_dirs:
    return

  x64_runtime, x86_runtime = vs_runtime_dll_dirs
  runtime_dir = x64_runtime if target_cpu == 'x64' else x86_runtime
  _CopyRuntime(target_dir, runtime_dir, target_cpu, debug=False)
  if configuration == 'Debug':
    _CopyRuntime(target_dir, runtime_dir, target_cpu, debug=True)
  else:
    _CopyPGORuntime(target_dir, target_cpu)

  _CopyDebugger(target_dir, target_cpu)


def _CopyDebugger(target_dir, target_cpu):
  """Copy dbghelp.dll and dbgcore.dll into the requested directory as needed.

  target_cpu is one of 'x86' or 'x64'.

  dbghelp.dll is used when Chrome needs to symbolize stacks. Copying this file
  from the SDK directory avoids using the system copy of dbghelp.dll which then
  ensures compatibility with recent debug information formats, such as VS
  2017 /debug:fastlink PDBs.

  dbgcore.dll is needed when using some functions from dbghelp.dll (like
  MinidumpWriteDump).
  """
  win_sdk_dir = SetEnvironmentAndGetSDKDir()
  if not win_sdk_dir:
    return

  # List of debug files that should be copied, the first element of the tuple is
  # the name of the file and the second indicates if it's optional.
  debug_files = [('dbghelp.dll', False), ('dbgcore.dll', True)]
  for debug_file, is_optional in debug_files:
    full_path = os.path.join(win_sdk_dir, 'Debuggers', target_cpu, debug_file)
    if not os.path.exists(full_path):
      if is_optional:
        continue
      else:
        raise Exception('%s not found in "%s"\r\nYou must install the '
                        '"Debugging Tools for Windows" feature from the Windows'
                        ' 10 SDK.' % (debug_file, full_path))
    target_path = os.path.join(target_dir, debug_file)
    _CopyRuntimeImpl(target_path, full_path)


def _GetDesiredVsToolchainHashes():
  """Load a list of SHA1s corresponding to the toolchains that we want installed
  to build with."""
  env_version = GetVisualStudioVersion()
  if env_version == '2015':
    # Update 3 final with 10.0.15063.468 SDK and no vctip.exe.
    return ['f53e4598951162bad6330f7a167486c7ae5db1e5']
  if env_version == '2017':
    # VS 2017 Update 3.2 with 10.0.15063.468 SDK and patched setenv.cmd.
    return ['a9e1098bba66d2acccc377d5ee81265910f29272']
  raise Exception('Unsupported VS version %s' % env_version)


def NormalizePath(path):
  while path.endswith("\\"):
    path = path[:-1]
  return path


def SetEnvironmentAndGetSDKDir():
  """Gets location information about the current sdk (must have been
  previously updated by 'update'). This is used for the GN build."""
  SetEnvironmentAndGetRuntimeDllDirs()

  # If WINDOWSSDKDIR is not set, search the default SDK path and set it.
  if not 'WINDOWSSDKDIR' in os.environ:
    default_sdk_path = os.path.join(os.environ.get('ProgramFiles(x86)'), 'Windows Kits', '10')
    if os.path.isdir(default_sdk_path):
      os.environ['WINDOWSSDKDIR'] = default_sdk_path

  return NormalizePath(os.environ['WINDOWSSDKDIR'])


def GetToolchainDir():
  """Gets location information about the current toolchain (must have been
  previously updated by 'update'). This is used for the GN build."""
  runtime_dll_dirs = SetEnvironmentAndGetRuntimeDllDirs()
  win_sdk_dir = SetEnvironmentAndGetSDKDir()

  print('''vs_path = "%s"
sdk_path = "%s"
vs_version = "%s"
wdk_dir = "%s"
runtime_dirs = "%s"
''' % (
      NormalizePath(os.environ['VISUAL_STUDIO_PATH']),
      win_sdk_dir,
      GetVisualStudioVersion(),
      NormalizePath(os.environ.get('WDK_DIR', '')),
      os.path.pathsep.join(runtime_dll_dirs or ['None'])))


def main():
  commands = {
      'get_toolchain_dir': GetToolchainDir,
      'copy_dlls': CopyDlls,
  }
  if len(sys.argv) < 2 or sys.argv[1] not in commands:
    print('Expected one of: %s' % ', '.join(commands), file=sys.stderr)
    return 1
  return commands[sys.argv[1]](*sys.argv[2:])


if __name__ == '__main__':
  sys.exit(main())
