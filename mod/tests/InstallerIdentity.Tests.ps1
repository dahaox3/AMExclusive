$ErrorActionPreference = 'Stop'
$script:ameChinese = $false
function Get-InstallerText([string]$Chinese,[string]$English) { return $English }
$scriptPath = Join-Path $PSScriptRoot '../scripts/Register-Companion.ps1'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($scriptPath,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-RegisteredTaskPrincipalSid'},$true)
Invoke-Expression $function.Extent.Text
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
# Account/computer display names are deliberately not supplied to the resolver.
$xml="<Task xmlns='http://schemas.microsoft.com/windows/2004/02/mit/task'><Principals><Principal><UserId>$sid</UserId></Principal></Principals></Task>"
if ((Get-RegisteredTaskPrincipalSid $xml) -ne $sid) { throw 'XML SID verification failed' }
foreach ($bad in @('<Task/>', '<Task><Principals><Principal><UserId>DAHAO</UserId></Principal></Principals></Task>')) {
    $rejected=$false
    try { Get-RegisteredTaskPrincipalSid $bad | Out-Null } catch { $rejected=$true }
    if (-not $rejected) { throw 'Ambiguous display-name principal was accepted' }
}
# Read-only integration check of the existing task, when installed.
$service=New-Object -ComObject Schedule.Service
$service.Connect()
try { $task=$service.GetFolder('\').GetTask("AMExclusive-Companion-$sid") } catch { $task=$null }
if ($task -and (Get-RegisteredTaskPrincipalSid $task.Xml) -ne $sid) { throw 'Persisted task SID mismatch' }
Write-Output 'Installer identity regressions passed (including existing task XML when available)'
