# Creates (or reuses) the self-signed code-signing certificate used to test-sign
# FFPanel.sys, and makes it trusted on this machine.
# Prints the thumbprint on stdout so install_driver.bat can capture it.

$ErrorActionPreference = 'Stop'
$subject = 'CN=FFPanelTest'

$cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date) } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert `
                -Subject $subject `
                -CertStoreLocation Cert:\LocalMachine\My `
                -NotAfter (Get-Date).AddYears(5)
}

$cerPath = Join-Path $env:TEMP 'FFPanelTest.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null

foreach ($store in @('Root', 'TrustedPublisher')) {
    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
}

Write-Output $cert.Thumbprint
