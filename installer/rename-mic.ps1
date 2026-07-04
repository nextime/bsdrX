<#
  Rename the VB-CABLE audio endpoints to "BSRD_Mic" so that:
    - other apps see the recording endpoint (VB-CABLE "Output") as a mic named BSRD_Mic
    - the bsdrX agent finds the render endpoint (VB-CABLE "Input") by the name BSRD_Mic
      (audio_wasapi.c matches the render device whose friendly name contains BSRD_Mic)

  Both VB-CABLE endpoints (one Render, one Capture) are renamed to the same
  friendly name; they live under different registry roots so this is fine.

  Run elevated (the installer does). Audio service is restarted so the new name
  shows immediately.
#>
param([string]$NewName = "BSRD_Mic")

# PKEY_Device_FriendlyName  = "{a45c254e-df1c-4efd-8020-67d146a850e0},2"
# PKEY_DeviceInterface_FriendlyName = "{026e516e-b814-414b-83cd-856d6fef4822},2"  (endpoint label)
$FriendlyKey = "{a45c254e-df1c-4efd-8020-67d146a850e0},2"
$Roots = @(
  "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render",
  "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture"
)

$renamed = 0
foreach ($root in $Roots) {
  if (-not (Test-Path $root)) { continue }
  Get-ChildItem $root | ForEach-Object {
    $props = Join-Path $_.PSPath "Properties"
    if (-not (Test-Path $props)) { return }
    try {
      $name = (Get-ItemProperty -Path $props -Name $FriendlyKey -ErrorAction Stop).$FriendlyKey
    } catch { return }
    if ($name -and $name -match "CABLE") {
      Set-ItemProperty -Path $props -Name $FriendlyKey -Value $NewName
      Write-Host "Renamed '$name' -> '$NewName'  [$($_.PSChildName)]"
      $renamed++
    }
  }
}

if ($renamed -eq 0) {
  Write-Warning "No VB-CABLE endpoints found. Is VB-CABLE installed?"
} else {
  # bounce the audio service so the new name is picked up immediately
  try { Restart-Service -Name Audiosrv -Force -ErrorAction Stop; Write-Host "Audio service restarted." }
  catch { Write-Warning "Could not restart Audiosrv; a reboot will apply the new name." }
}
exit 0
