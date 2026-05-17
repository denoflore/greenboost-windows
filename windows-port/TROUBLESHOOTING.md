# GreenBoost Windows — Troubleshooting

## Driver Issues

### "Cannot find any service with service name 'GreenBoost'" / "Service didn't find" (Issue #10)

This is the most common install failure. The installer ran and printed
"Installation Complete" (in older versions before the v2.3.1 fix), but after
a reboot `Restart-Service GreenBoost` or `sc query GreenBoost` reports the
service does not exist.

**Root cause**: `devcon install` did not register the driver service. The
99% case is that **test-signing is not enabled** in the Boot Configuration
Database. The driver is test-signed; Windows refuses to load a test-signed
driver unless the OS is in test-signing mode.

**Diagnose**:

```powershell
# Is test-signing on?
bcdedit /enum "{current}" | findstr testsigning
# Expected on a working install:  testsigning             Yes
```

If you see `testsigning             No` (or no `testsigning` line at all),
that's the cause.

**Fix**:

```powershell
# Run in an elevated PowerShell:
bcdedit /set testsigning on

# Reboot.
# After reboot you should see a "Test Mode" watermark in the bottom-right.
# That's correct — it means test-signed drivers can now load.

# Then re-run the installer:
cd <repo>\windows-port\tools
.\install.ps1
```

If test-signing is ALREADY on and the service still doesn't register, run
through this checklist in order:

1. **Re-sign the driver.** The INF may have lost its signature info.
   ```powershell
   .\sign.ps1
   ```

2. **Confirm KMDF coinstaller is present.** Look for `WdfCoInstaller01011.dll`
   (or similar version) in the same folder as `greenboost_win.sys`. If
   missing, install the Windows Driver Kit (WDK) — it ships the coinstaller.

3. **Trust the test certificate.** The first time a test-signed driver loads
   on a clean machine, the cert root may not be trusted yet.
   ```powershell
   # Inspect cert with:
   Get-AuthenticodeSignature outputs\greenboost_win.sys | Format-List
   # If the certificate's Subject isn't in Trusted Root, import via certmgr.msc
   # Local Computer → Trusted Root Certification Authorities → Certificates → Import
   ```

4. **Try Disable Driver Signature Enforcement (DSE) from boot menu** for a
   one-time test:
   ```
   Shift + Restart → Troubleshoot → Advanced options → Startup Settings
   → Restart → press 7 (Disable driver signature enforcement)
   ```
   This boots Windows in a mode that will load the driver. If the service
   appears in this mode, the issue is definitely signature trust.

5. **Check Event Viewer** for driver-load failures:
   ```
   Event Viewer → Windows Logs → System
   Source: "Service Control Manager" or "Kernel-PnP"
   Look for entries near the timestamp of the failed install.
   ```

The installer in v2.3.1+ surfaces this diagnosis automatically and refuses
to print "Installation Complete" when the service doesn't register.

---

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

- The driver does not grant World/Everyone access. Its device SDDL grants:
  - Local System: generic all
  - Built-in Administrators: generic all
  - Interactive Users: generic read/write
- If access is still denied:
  - Run the application from an interactive user session or as Administrator.
  - Check if security software is blocking device access.

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

### Driver-backed allocation mapping fails

- The current shim path does not use `MapViewOfFile`.
- The driver maps pinned pages into the calling process with
  `MmMapLockedPagesSpecifyCache`, then the shim registers that host pointer
  with CUDA.
- If driver-backed allocation fails, check driver debug output with
  `dbgview.exe` (Sysinternals DebugView) and confirm the driver is running and
  accepting IOCTLs.

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
