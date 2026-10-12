"""Run a command with a cap on the memory it and everything it starts may use (Windows).

Usage: memcap.py [--limit 10G] [--] command [args...]

The command runs inside a Windows job object with a job-wide committed-memory limit
(JOB_OBJECT_LIMIT_JOB_MEMORY): every process it starts belongs to the job, and allocations
past the limit fail in the process that makes them instead of starving the PC. The job's
processes are killed when this wrapper ends. The exit code is the command's.
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
kernel32.CreateJobObjectW.restype = wt.HANDLE
kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wt.LPCWSTR]
kernel32.GetCurrentProcess.restype = wt.HANDLE
kernel32.AssignProcessToJobObject.argtypes = [wt.HANDLE, wt.HANDLE]
kernel32.SetInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD]
kernel32.QueryInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p]

JobObjectExtendedLimitInformation = 9
JOB_OBJECT_LIMIT_JOB_MEMORY = 0x200
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ('ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                                                  'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                ('LimitFlags', wt.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wt.DWORD),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', wt.DWORD), ('SchedulingClass', wt.DWORD)]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [('BasicLimitInformation', JOBOBJECT_BASIC_LIMIT_INFORMATION), ('IoInfo', IO_COUNTERS),
                ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]


def parse_size(text):
    text = text.strip().upper()
    scale = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}.get(text[-1:], 1)
    return int(float(text[:-1] if text[-1:] in 'KMG' else text) * scale)


def main(argv):
    limit = 10 << 30
    if argv[:1] == ['--limit']:
        limit, argv = parse_size(argv[1]), argv[2:]
    if argv[:1] == ['--']:
        argv = argv[1:]
    if not argv:
        sys.exit(__doc__)
    job = kernel32.CreateJobObjectW(None, None)
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    info.JobMemoryLimit = limit
    if not kernel32.SetInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(info),
                                            ctypes.sizeof(info)):
        sys.exit(f'memcap: SetInformationJobObject failed ({ctypes.get_last_error()})')
    # this process joins the job, so the command and everything it starts are in it too
    if not kernel32.AssignProcessToJobObject(job, kernel32.GetCurrentProcess()):
        sys.exit(f'memcap: AssignProcessToJobObject failed ({ctypes.get_last_error()})')
    code = subprocess.call(argv)
    kernel32.QueryInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(info),
                                       ctypes.sizeof(info), None)
    print(f'memcap: peak {info.PeakJobMemoryUsed / (1 << 30):.2f} GB of {limit / (1 << 30):.1f} GB', file=sys.stderr)
    sys.exit(code)


if __name__ == '__main__':
    main(sys.argv[1:])
