Get-WmiObject Win32_PnPEntity | Where-Object { $_.Name -like "*GreenBoost*" } | Format-List *
Write-Host "--- Device Manager Entries ---"
pnputil /enum-drivers | Select-String -Pattern "GreenBoost" -Context 5,5