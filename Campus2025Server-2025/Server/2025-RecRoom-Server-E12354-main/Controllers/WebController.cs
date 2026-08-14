using Microsoft.AspNetCore.Mvc;
using Newtonsoft.Json;
using Campus_live_rewrite.api.server.Classes.db_class;
using Campus_live_rewrite.api.server;
using System.ComponentModel.DataAnnotations;

namespace Campus_live_rewrite.Controllers
{
    public class WebController : ControllerBase
    {
        // "/" MUST return the NameServer response (JSON) - the July 19 2025 Rec Room client
        // treats "/" as the NS endpoint and rejects anything that isn't a valid NS DTO with
        // FAILED_TO_CONNECT_CODE_LOCKER (=4, "Failed to connect to RecNet, error code: locker").
        // The Campus landing page moved to /home so browsers can still visit the site.
        [Route("/")]
        public IActionResult NsRoot() => ns(Version: "20250731");

        [Route("/home")]
        [Route("/site")]
        public IActionResult Index()
        {
            var filePath = Path.Combine(Environment.CurrentDirectory, "Web", "Index.html");

            if (!System.IO.File.Exists(filePath))
                return NotFound();

            return PhysicalFile(filePath, "text/html");
        }

        [Route("/robots.txt")]
        public IActionResult Robots()
        {
            var content = string.Join(Environment.NewLine, new[]
            {
                "User-agent: *",
                "Disallow: /admin/v4/login",
                "Disallow: /admin/v4/home",
                "Disallow: /admin/v5/login",
                "Disallow: /admin/v5/home"
            });

            return Content(content, "text/plain");
        }

        [HttpGet("/favicon.ico")]
        public IActionResult Favicon()
        {
            // Serve the Campus AnimatedIcon as the browser-tab icon for gameauth.ddns.net.
            // Falls back to Web/favicon.png if the admin-assets file isn't shipped for some reason.
            var animated = Path.Combine(Environment.CurrentDirectory, "AdminPanel", "Assets", "AnimatedIcon.png");
            if (System.IO.File.Exists(animated))
                return PhysicalFile(animated, "image/png");

            var legacy = Path.Combine(Environment.CurrentDirectory, "Web", "favicon.png");
            if (System.IO.File.Exists(legacy))
                return PhysicalFile(legacy, "image/png");

            return NotFound();
        }

        [HttpGet("/ns")]
        public IActionResult ns([FromQuery(Name = "v")] string? Version = null)
        {
            // Reference server only served for "20250731" (July 31 2025 build). We serve the same
            // body for any/no version so older + newer client builds don't 400 here.
            {
                return Ok(new
                {
                    Accounts = "https://gameauth.ddns.net/Accounts",
                    API = "https://gameauth.ddns.net",
                    AI = "https://gameauth.ddns.net/Ai",
                    Auth = "https://gameauth.ddns.net/Auth",
                    BugReporting = "https://gameauth.ddns.net",
                    Cards = "https://gameauth.ddns.net",
                    // Was `https://cdn.rec.net` (a leftover in the E12354 base that pointed at
                    // real Rec Room's CDN to "save on our own bandwidth"). The July 19 2025
                    // client's `summer` NS-validation gate checks every URL's host and fails
                    // when they don't all resolve back to us - mate's working server puts CDN
                    // on his host, so we do the same. 2026-08-13.
                    CDN = "https://gameauth.ddns.net/cdn",
                    Chat = "https://gameauth.ddns.net/chat",
                    Clubs = "https://gameauth.ddns.net/clubs",
                    CMS = "https://gameauth.ddns.net/cms",
                    Commerce = "https://gameauth.ddns.net/Commerce",
                    Data = "https://gameauth.ddns.net",
                    DataCollection = "https://gameauth.ddns.net",
                    Discovery = "https://gameauth.ddns.net",
                    Econ = "https://gameauth.ddns.net",
                    GameLogs = "https://gameauth.ddns.net",
                    Geo = "https://gameauth.ddns.net",
                    Images = "https://gameauth.ddns.net/img/",
                    Leaderboard = "https://gameauth.ddns.net/server/Leaderboard",
                    Link = "https://gameauth.ddns.net/link",
                    Lists = "https://gameauth.ddns.net/lists",
                    Matchmaking = "https://gameauth.ddns.net/Matchmaking",
                    Moderation = "https://gameauth.ddns.net",
                    Notifications = "https://gameauth.ddns.net/Notifications",
                    PlatformNotifications = "https://gameauth.ddns.net/Notifications",
                    PlayerSettings = "https://gameauth.ddns.net/server/PlayerSettings",
                    RoomComments = "https://gameauth.ddns.net/RoomComments",
                    RoomieIntegrations = "https://gameauth.ddns.net/Roomie",
                    Rooms = "https://gameauth.ddns.net/Room_server",
                    Storage = "https://gameauth.ddns.net/server/Storage",
                    Strings = "https://gameauth.ddns.net/Strings",
                    StringsCDN = "https://gameauth.ddns.net/StringsCDN",
                    Studio = "https://gameauth.ddns.net/Studio",
                    Thorn = "https://gameauth.ddns.net/Thorn",
                    Videos = "https://gameauth.ddns.net/videodir",
                    WWW = "https://gameauth.ddns.net"
                });
            }
        }
    }
}
