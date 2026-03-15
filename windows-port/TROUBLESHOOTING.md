# GreenBoost Windows — Troubleshooting

## Driver Issues

### "Device not found" / CreateFile fails

1. Check if driver service is registered:
   ```powershell
   sc query GreenBoost
   ```

2. Check if device exists in Device Manager:
   - Open Device Manager → System devices → "GreenBoost GPU Memory Extension"

3. Install the driver:
   ```powershell
   pnputil /add-driver driver\greenboost_win.inf /install
   ```

4. Start the service:
   ```powershell
   sc start GreenBoost
   ```

### "Access denied" opening device

- The driver's SDDL allows Everyone read/write access. If access is still denied:
  - Run your application as Administrator
  - Check if security software is blocking device access

### BSOD on driver load

1. Enable Driver Verifier:
   ```powershell
   verifier /standard /driver greenboost_win.sys
   ```

2. Check minidump: `C:\Windows\Minidump\*.dmp`

3. Common causes:
   - Memory allocation failure not handled (check system RAM)
   - Spinlock held too long (watchdog thread)
   - MDL operations on invalid addresses

### Test signing not enabled

```powershell
# Enable (requires reboot)
bcdedit /set testsigning on

# Verify
bcdedit /enum "{current}" | findstr testsigning
```

## Shim Issues

### "cuMemHostRegister not resolved"

- NVIDIA driver may be too old. Requires driver 535+ (CUDA 12+).
- Check: `nvidia-smi` should show driver version.

### Allocations not being intercepted

1. Enable debug logging:
   ```powershell
   $env:GREENBOOST_DEBUG = "1"
   withdll.exe /d:greenboost_cuda.dll your_app.exe
   ```

2. Check threshold: default 256MB. Allocations below this pass through.
   ```powershell
   # Lower threshold for testing
   $env:GREENBOOST_THRESHOLD_MB = "64"
   ```

3. Verify shim is loaded:
   ```powershell
   # Check loaded modules in process
   Get-Process -Name "your_app" | ForEach-Object {
       $_.Modules | Where-Object { $_.ModuleName -like "*greenboost*" }
   }
   ```

### "MapViewOfFile failed"

- Section handle from driver may be invalid
- Check driver debug output: `dbgview.exe` (Sysinternals DebugView)
- Ensure driver is running and accepting IOCTLs

### CUDA app crashes on startup

- Shim is designed to gracefully fall through on any failure
- If crashes occur, the shim may be conflicting with the CUDA runtime
- Try: `$env:GREENBOOST_DEBUG = "1"` to see where the failure occurs
- Ensure the correct nvcuda.dll is loaded (not a renamed copy)

### LM Studio doesn't see extended VRAM

1. Check registry config:
   ```powershell
   Get-ItemProperty HKLM:\SOFTWARE\GreenBoost
   ```

2. Verify with test tool:
   ```powershell
   build\tests\Release\test_ioctl.exe
   ```

3. Check that LM Studio is launched with the shim:
   ```powershell
   ~\.cache\lm-studio\greenboost_launch.bat
   ```

## Memory Pressure

### OOM guard tripping frequently

- Increase safety reserve:
  ```powershell
  Set-ItemProperty HKLM:\SOFTWARE\GreenBoost -Name SafetyReserveGb -Value 16
  ```

- Reduce virtual VRAM cap:
  ```powershell
  Set-ItemProperty HKLM:\SOFTWARE\GreenBoost -Name VirtualVramGb -Value 40
  ```

### Pagefile / swap pressure warnings

- Ensure pagefile is on NVMe (not HDD)
- Increase pagefile size: System Properties → Advanced → Performance → Virtual Memory
- Check with: `tools\diagnose.ps1`

## Diagnostic Checklist

Run `tools\diagnose.ps1` for automated checks. Manual checklist:

1. [ ] `nvidia-smi` reports GPU and driver version
2. [ ] `bcdedit` shows test signing enabled
3. [ ] `sc query GreenBoost` shows RUNNING
4. [ ] `test_ioctl.exe` passes all tests
5. [ ] `diagnose.ps1` shows all PASS
6. [ ] `GREENBOOST_DEBUG=1` shows shim initialization
7. [ ] `cuDeviceTotalMem` reports extended VRAM in debug output
