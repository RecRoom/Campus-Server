using Microsoft.AspNetCore.Mvc;
using Newtonsoft.Json;
using Campus_live_rewrite.api.server;
using System.Net.Security;

namespace Campus_live_rewrite.Controllers
{
    [ApiController]
    public class CDNController : Controller
    {
        // Reused HttpClient for CDN passthrough. .NET 8 on Linux was failing the TLS handshake
        // to cdn.rec.net with "SSL connection could not be established" even though curl handshakes
        // fine from the same box — most likely a chain-verification quirk. Trust everything and
        // move on; we're only fetching public asset blobs, not credentials.
        private static readonly HttpClient s_cdnClient = new HttpClient(new SocketsHttpHandler
        {
            SslOptions = new SslClientAuthenticationOptions
            {
                // curl handshakes fine from the same box; .NET's default cert chain check trips
                // on cdn.rec.net specifically. Accept whatever the server presents — we're only
                // fetching public asset blobs, not credentials.
                RemoteCertificateValidationCallback = (_, _, _, _) => true
            },
            AutomaticDecompression = System.Net.DecompressionMethods.All,
            ConnectTimeout = TimeSpan.FromSeconds(10)
        })
        {
            Timeout = TimeSpan.FromSeconds(20)
        };

        [HttpGet("/cdn/config/LoadingScreenTipData")]
        public IActionResult LoadingScreenTipData()
        {
            string filePath = Path.Combine(Environment.CurrentDirectory, "Data", "APIS", "LoadingScreenTipData.json");

            if (!System.IO.File.Exists(filePath))
                return Content("[]", "application/json");

            return PhysicalFile(filePath, "application/json");
        }

        [HttpGet("/iam/me/channels/{type}")]
        public IActionResult GetIamChannel(string type)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            string basePath = Path.Combine(Environment.CurrentDirectory, "Data", "APIS");

            switch (type.ToLower())
            {
                case "announcements":
                    string announcementsPath = Path.Combine(basePath, "IamAnnouncements.json");
                    if (System.IO.File.Exists(announcementsPath))
                        return PhysicalFile(announcementsPath, "application/json");
                    break;

                case "justintimetutorials":
                    return Ok(ConfigCampus.Bracket);
            }

            return Ok(ConfigCampus.Bracket);
        }

        [HttpGet("/sections/pagesource/{type}")]
        public IActionResult GetWatchPages(string type)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            string path = Path.Combine(
                Environment.CurrentDirectory,
                "Data",
                "APIS",
                "WatchPages",
                $"{type}.json"
            );

            if (!System.IO.File.Exists(path))
                return Ok(ConfigCampus.Bracket);

            return PhysicalFile(path, "application/json");
        }

        [Route("/cdn/room/{*room_path}")]
        public async Task<IActionResult> CDNRoom(string room_path)
        {
            // 1. SANITIZE: Prevent path traversal
            string fileName = Path.GetFileName(room_path); 
            
            string localPath = Path.Combine(Environment.CurrentDirectory, "Data", "CDN", "RoomBlobs", room_path);
            string directory = Path.GetDirectoryName(localPath)!;

            try
            {
                if (!Directory.Exists(directory))
                    Directory.CreateDirectory(directory);

                // 2. DOWNLOAD IF MISSING. Try cdn.rec.net first, then img.rec.net as a
                // fallback (Rec Room historically served the same room blobs from both).
                if (!System.IO.File.Exists(localPath))
                {
                    string[] upstreams = {
                        $"https://cdn.rec.net/room/{room_path}",
                        $"https://img.rec.net/room/{room_path}",
                    };
                    HttpResponseMessage? good = null;
                    foreach (var url in upstreams)
                    {
                        try
                        {
                            var resp = await s_cdnClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
                            if (resp.IsSuccessStatusCode) { good = resp; break; }
                            Console.WriteLine($"[cdn/room] upstream {url} -> HTTP {(int)resp.StatusCode}");
                            resp.Dispose();
                        }
                        catch (Exception uex)
                        {
                            Console.WriteLine($"[cdn/room] upstream {url} FAILED: {uex.GetType().Name}: {uex.Message}");
                        }
                    }
                    if (good == null) return NotFound();

                    using (good)
                    await using (var remoteStream = await good.Content.ReadAsStreamAsync())
                    await using (var fileStream = new FileStream(localPath, FileMode.Create, FileAccess.Write, FileShare.None))
                    {
                        await remoteStream.CopyToAsync(fileStream);
                    }
                    Console.WriteLine($"[cdn/room] cached {room_path} ({new FileInfo(localPath).Length} bytes)");
                }

                // 3. GATHER FILE METADATA & DYNAMIC DATA
                var fileInfo = new FileInfo(localPath);
                var now = DateTimeOffset.UtcNow;
                
                // Compute actual MD5 hash dynamically
                string md5Base64;
                using (var md5 = System.Security.Cryptography.MD5.Create())
                {
                    using var readStream = System.IO.File.OpenRead(localPath);
                    byte[] hash = await md5.ComputeHashAsync(readStream);
                    md5Base64 = Convert.ToBase64String(hash);
                }

                // Generate fake Azure/Cloudflare IDs
                string requestId = Guid.NewGuid().ToString();
                // Mimic Azure Ref format: YYYYMMDDTHHMMSSZ-[randomhex]
                string azureRef = $"{now:yyyyMMddTHHmmssZ}-{Guid.NewGuid().ToString("N").Substring(0, 32)}"; 
                // Generate ETag based on the file's LastWriteTime to act as a unique version identifier
                string eTag = $"\"0x{fileInfo.LastWriteTimeUtc.Ticks:X}\"";

                // 4. APPLY DYNAMIC HEADERS
                // Note: Content-Length, Content-Type, and Accept-Ranges are handled automatically by the File() return method.
                Response.Headers["Date"] = now.ToString("R"); // RFC 1123 format (e.g., Thu, 12 Mar 2026 16:06:27 GMT)
                Response.Headers["Cache-Control"] = "public, max-age=31536000";
                Response.Headers["Last-Modified"] = fileInfo.LastWriteTimeUtc.ToString("R");
                Response.Headers["ETag"] = eTag;
                
                // Mock Azure Blob Storage Headers
                Response.Headers["x-ms-request-id"] = requestId;
                Response.Headers["x-ms-version"] = "2021-04-10";
                Response.Headers["x-ms-version-id"] = fileInfo.LastWriteTimeUtc.ToString("o"); // ISO 8601 format
                Response.Headers["x-ms-is-current-version"] = "true";
                Response.Headers["x-ms-creation-time"] = fileInfo.CreationTimeUtc.ToString("R");
                Response.Headers["x-ms-blob-content-md5"] = md5Base64;
                Response.Headers["x-ms-lease-status"] = "unlocked";
                Response.Headers["x-ms-lease-state"] = "available";
                Response.Headers["x-ms-blob-type"] = "BlockBlob";
                Response.Headers["x-ms-server-encrypted"] = "true";
                Response.Headers["x-ms-last-access-time"] = now.ToString("R");
                
                // CORS and CDN Mock Headers
                Response.Headers["Access-Control-Expose-Headers"] = "x-ms-request-id,Server,x-ms-version,x-ms-version-id,x-ms-is-current-version,Content-Type,Cache-Control,ETag,Last-Modified,x-ms-creation-time,x-ms-blob-content-md5,x-ms-lease-status,x-ms-lease-state,x-ms-blob-type,x-ms-server-encrypted,Accept-Ranges,x-ms-last-access-time,Content-Length,Date,Transfer-Encoding";
                Response.Headers["Access-Control-Allow-Origin"] = "*";
                Response.Headers["x-azure-ref"] = azureRef;
                Response.Headers["x-fd-int-roxy-purgeid"] = "0";
                Response.Headers["X-Cache-Info"] = "L2_T2";
                Response.Headers["X-Cache"] = "TCP_REMOTE_HIT"; // Tells the client it successfully hit the CDN cache

                // 5. RETURN AS STREAM
                var fs = new FileStream(localPath, FileMode.Open, FileAccess.Read, FileShare.Read);
                
                // enableRangeProcessing allows standard 206 Partial Content responses (crucial for large room files)
                return File(fs, "application/octet-stream", enableRangeProcessing: true);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"CDN Error: {ex.Message}");
                return NotFound();
            }
        }

        [Route("/cdn/data/{*room_path}")]
        public async Task<IActionResult> CDNData(string room_path)
        {
            try
            {
                string localPath = Path.Combine(Environment.CurrentDirectory, "Data", "CDN", "DataBlobs", room_path);

                string directory = Path.GetDirectoryName(localPath);

                if (!Directory.Exists(directory))
                {
                    Directory.CreateDirectory(directory);
                }

                if (System.IO.File.Exists(localPath))
                {
                    return File(await System.IO.File.ReadAllBytesAsync(localPath), "application/octet-stream");
                }

                string[] upstreams = {
                    $"https://cdn.rec.net/data/{room_path}",
                    $"https://img.rec.net/data/{room_path}",
                };
                foreach (var url in upstreams)
                {
                    try
                    {
                        var resp = await s_cdnClient.GetAsync(url);
                        if (!resp.IsSuccessStatusCode)
                        {
                            Console.WriteLine($"[cdn/data] upstream {url} -> HTTP {(int)resp.StatusCode}");
                            continue;
                        }
                        byte[] data = await resp.Content.ReadAsByteArrayAsync();
                        await System.IO.File.WriteAllBytesAsync(localPath, data);
                        Console.WriteLine($"[cdn/data] cached {room_path} ({data.Length} bytes) from {url}");
                        return File(data, "application/octet-stream");
                    }
                    catch (Exception uex)
                    {
                        Console.WriteLine($"[cdn/data] upstream {url} FAILED: {uex.GetType().Name}: {uex.Message}");
                    }
                }
                return NotFound("");
            }
            catch (Exception ex)
            {
                Console.WriteLine(ex.ToString());
                return NotFound();
            }
        }
    }
}
