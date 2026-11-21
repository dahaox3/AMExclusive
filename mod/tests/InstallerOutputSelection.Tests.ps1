$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$path = Join-Path $PSScriptRoot '../scripts/Install-Mod.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$function = $ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and
                                $n.Name -eq 'Get-PreviousOutputSelection'}, $true)
Invoke-Expression $function.Extent.Text
function Check-Selection([string[]]$Lines, [string]$Expected) {
    $actual = Get-PreviousOutputSelection $Lines
    if ($actual -cne $Expected) { throw "Expected '$Expected', got '$actual'" }
}
$id = '{C85B4969-F231-4444-ABCD-AB072A36B21F}'
$endpoint = '{0.0.0.00000000}.{00000000-0000-0000-0000-000000000001}'
Check-Selection @() 'shared:'
Check-Selection @('mode=exclusive', "asio_driver=$($id.ToLowerInvariant())") "exclusive:asio:$id"
Check-Selection @("asio_driver=$id", 'mode=probe') "shared:asio:$id"
Check-Selection @("asio_driver=$id", 'mode=exclusive', 'output_selection=shared:') 'shared:'
Check-Selection @("output_selection=exclusive:$endpoint") "exclusive:$endpoint"
Check-Selection @('output_selection=shared:opaque$1=') 'shared:opaque$1='
Check-Selection @('output_selection=exclusive:asio:broken') 'shared:'
Check-Selection @(('output_selection=exclusive:' + ('A' * 256))) 'shared:'
Write-Output 'Output selection upgrade and explicit default tests passed.'
