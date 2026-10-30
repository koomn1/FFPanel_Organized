# Removes the CN=FFPanelTest certificate from every store install_driver.bat
# touched. Used by: uninstall_driver.bat --remove-cert

$ErrorActionPreference = 'SilentlyContinue'
$subject = 'CN=FFPanelTest'
$removed = 0

foreach ($store in @('My', 'Root', 'TrustedPublisher')) {
    $path = "Cert:\LocalMachine\$store"
    Get-ChildItem $path |
        Where-Object { $_.Subject -eq $subject } |
        ForEach-Object {
            Remove-Item -Path (Join-Path $path $_.Thumbprint) -Force
            $removed++
        }
}

Write-Output "removed $removed certificate(s)"
