# Serves scripts/grok-relay on localhost so a browser can run test/run.html.
param([int]$Port = 8791)
$root = Split-Path -Parent $PSScriptRoot
$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
$types = @{ ".html" = "text/html"; ".js" = "text/javascript"; ".sql" = "text/plain" }
while ($listener.IsListening) {
    $context = $listener.GetContext()
    $path = Join-Path $root ($context.Request.Url.LocalPath.TrimStart("/"))
    if (Test-Path $path -PathType Leaf) {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $context.Response.ContentType = $types[[System.IO.Path]::GetExtension($path)]
        $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    } else {
        $context.Response.StatusCode = 404
    }
    $context.Response.Close()
}
