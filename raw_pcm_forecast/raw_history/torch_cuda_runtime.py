
import ctypes as ct
import os
from pathlib import Path
import torch


class BundledCuda:
    def __init__(self, source_path):
        torch.cuda.init()
        torch.empty(0, device="cuda")
        directory = Path(torch.__file__).parent / "lib"
        self.search_path = os.add_dll_directory(str(directory))
        candidates = sorted(p for p in directory.glob("nvrtc64_*.dll") if ".alt." not in p.name)
        if not candidates:
            raise RuntimeError("The installed CUDA runtime has no NVRTC compiler")
        self.rtc = ct.CDLL(str(candidates[0]))
        self.driver = ct.WinDLL("nvcuda.dll")
        self._bind()
        program = ct.c_void_p()
        source = Path(source_path).read_bytes()
        self._rtc(self.rtc.nvrtcCreateProgram(ct.byref(program), source,
                                             Path(source_path).name.encode(), 0, None, None))
        major, minor = torch.cuda.get_device_capability()
        options = [f"--gpu-architecture=compute_{major}{minor}".encode(), b"--std=c++14"]
        try:
            status = self.rtc.nvrtcCompileProgram(program, len(options),
                                                  (ct.c_char_p * len(options))(*options))
            size = ct.c_size_t()
            self.rtc.nvrtcGetProgramLogSize(program, ct.byref(size))
            log = ct.create_string_buffer(size.value)
            self.rtc.nvrtcGetProgramLog(program, log)
            if status:
                raise RuntimeError(log.value.decode(errors="replace"))
            self._rtc(self.rtc.nvrtcGetPTXSize(program, ct.byref(size)))
            ptx = ct.create_string_buffer(size.value)
            self._rtc(self.rtc.nvrtcGetPTX(program, ptx))
            self.module = ct.c_void_p()
            self._cuda(self.driver.cuModuleLoadData(ct.byref(self.module), ptx))
        finally:
            self.rtc.nvrtcDestroyProgram(ct.byref(program))
        self.functions = {}

    def _bind(self):
        signatures = {
            "nvrtcCreateProgram": [ct.POINTER(ct.c_void_p), ct.c_char_p, ct.c_char_p,
                                   ct.c_int, ct.c_void_p, ct.c_void_p],
            "nvrtcCompileProgram": [ct.c_void_p, ct.c_int, ct.POINTER(ct.c_char_p)],
            "nvrtcGetProgramLogSize": [ct.c_void_p, ct.POINTER(ct.c_size_t)],
            "nvrtcGetProgramLog": [ct.c_void_p, ct.c_void_p],
            "nvrtcGetPTXSize": [ct.c_void_p, ct.POINTER(ct.c_size_t)],
            "nvrtcGetPTX": [ct.c_void_p, ct.c_void_p],
            "nvrtcDestroyProgram": [ct.POINTER(ct.c_void_p)],
        }
        for name, args in signatures.items():
            function = getattr(self.rtc, name)
            function.argtypes, function.restype = args, ct.c_int
        self.driver.cuModuleLoadData.argtypes = [ct.POINTER(ct.c_void_p), ct.c_void_p]
        self.driver.cuModuleGetFunction.argtypes = [ct.POINTER(ct.c_void_p), ct.c_void_p, ct.c_char_p]
        self.driver.cuLaunchKernel.argtypes = [
            ct.c_void_p, ct.c_uint, ct.c_uint, ct.c_uint,
            ct.c_uint, ct.c_uint, ct.c_uint, ct.c_uint, ct.c_void_p,
            ct.POINTER(ct.c_void_p), ct.c_void_p]
        self.driver.cuGetErrorName.argtypes = [ct.c_int, ct.POINTER(ct.c_char_p)]

    @staticmethod
    def _rtc(status):
        if status:
            raise RuntimeError(f"NVRTC failed with status {status}")

    def _cuda(self, status):
        if status:
            name = ct.c_char_p()
            self.driver.cuGetErrorName(status, ct.byref(name))
            raise RuntimeError(f"CUDA failed: {name.value!r} ({status})")

    def launch(self, name, grid, block, arguments):
        if name not in self.functions:
            function = ct.c_void_p()
            self._cuda(self.driver.cuModuleGetFunction(ct.byref(function), self.module, name.encode()))
            self.functions[name] = function
        values = []
        for argument in arguments:
            if isinstance(argument, torch.Tensor):
                if not argument.is_cuda or not argument.is_contiguous():
                    raise ValueError("Native CUDA arguments must be contiguous CUDA tensors")
                values.append(ct.c_void_p(argument.data_ptr()))
            elif isinstance(argument, int):
                values.append(ct.c_int(argument))
            elif isinstance(argument, ct.c_int64):
                values.append(argument)
            else:
                raise TypeError(type(argument))
        parameters = (ct.c_void_p * len(values))(
            *(ct.cast(ct.pointer(value), ct.c_void_p).value for value in values))
        self._cuda(self.driver.cuLaunchKernel(
            self.functions[name], *grid, *block, 0,
            ct.c_void_p(torch.cuda.current_stream().cuda_stream), parameters, None))
