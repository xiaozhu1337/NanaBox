"""Compile and run snapshot tests against the Windows virtual disk provider."""

from pathlib import Path
import os
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def developer_environment():
    locator = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output(
        [str(locator), '-latest', '-products', '*', '-property', 'installationPath'],
        text=True, encoding='utf-8', errors='replace').strip()
    if not installation:
        raise RuntimeError('Install Visual Studio C++ tools before running these tests.')
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvars64.bat'
    output = subprocess.check_output(
        f'cmd.exe /d /s /c ""{setup}" >nul && set"',
        text=True, encoding='utf-8', errors='replace')
    return dict(line.split('=', 1) for line in output.splitlines()
                if '=' in line and not line.startswith('='))


def main():
    temporary = ROOT / '__tmp'
    temporary.mkdir(exist_ok=True)
    winrt = ROOT / 'Output/Objects/Release/NanaBox/x64/Generated Files'
    if not (winrt / 'winrt/base.h').exists():
        raise RuntimeError('Build NanaBox Release x64 once to generate C++/WinRT headers.')
    package = Path.home() / '.nuget/packages/mile.json/1.0.1057/Source/Native'
    executable = temporary / 'snapshot-tests.exe'
    data = temporary / ('snapshot-test-data-' + sys.argv[1] if len(sys.argv) > 1
                        else 'snapshot-test-data')
    if data.exists():
        raise RuntimeError(f'Test directory already exists; inspect it first: {data}')
    sources = [ROOT / 'Tests/Snapshots/SnapshotTests.cpp']
    sources += [ROOT / 'NanaBox/Snapshots' / name for name in
                ['SnapshotFiles.cpp', 'SnapshotStore.cpp', 'SnapshotMaintenance.cpp']]
    sources.append(package / 'Mile.Json.cpp')
    environment = developer_environment()
    compiler = Path(environment['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    command = [str(compiler), '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX',
               '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/DWINRT_NO_SOURCE_LOCATION',
               '/I' + str(winrt), '/I' + str(package),
               '/I' + str(ROOT / 'NanaBox/Snapshots'),
               '/Fe:' + str(executable), '/Fo:' + str(temporary) + '\\']
    command += [str(source) for source in sources]
    command += ['/link', 'virtdisk.lib', 'ole32.lib', 'oleaut32.lib',
                'uuid.lib', 'runtimeobject.lib', 'advapi32.lib']
    subprocess.run(command, cwd=ROOT, env=environment, check=True,
                   stdin=subprocess.DEVNULL)
    subprocess.run([str(executable), str(data)], cwd=ROOT, check=True,
                   stdin=subprocess.DEVNULL)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
