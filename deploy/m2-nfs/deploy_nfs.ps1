param(
    [string]$HostIp,
    [string]$HostPw = 'zlcats213',
    [int]$FirstId,
    [int]$LastId
)
$ErrorActionPreference = 'Continue'
. 'E:\Codex\2026-10-28\krg-p1-remote-exec\scripts\plinkcmd.ps1'

$list = Get-Content 'E:\Codex\2026-10-28\krg-p1-remote-exec\scripts\cluster.rpc.list' |
    Where-Object { $_.Trim() -ne '' }
$nodes = @{}
foreach ($l in $list) {
    $f = -split $l
    $nodes[[int]$f[0]] = [pscustomobject]@{ id = [int]$f[0]; ip = $f[1]; idx = [int]$f[2]; port = [int]$f[3] }
}

$script = 'E:\Codex\2026-10-28\krg-m2-nfs\work\join_krg.sh'
for ($nid = $FirstId; $nid -le $LastId; $nid++) {
    $n = $nodes[$nid]
    if ($n.ip -ne $HostIp) { continue }
    $port = 2300 + $n.idx
    $hk = Get-RemoteKey $n.ip $port 'zz213'
    if (-not $hk) { Write-Host "node $nid UNREACHABLE $($n.ip):$port"; continue }
    Copy-Remote $n.ip $port 'zz213' $hk $script '/tmp/join_krg.sh' 2>$null
    $r = Invoke-Remote $n.ip $port 'zz213' $hk 'sh /tmp/join_krg.sh'
    Write-Host "node $nid -> $r"
}
