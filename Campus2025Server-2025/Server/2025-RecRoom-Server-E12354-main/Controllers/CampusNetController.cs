using Microsoft.AspNetCore.Mvc;
using Newtonsoft.Json;
using Campus_live_rewrite.api.server.Classes.db_class;
using Campus_live_rewrite.api.server;
using Campus_live_rewrite.api.dbs;
using Newtonsoft.Json.Linq;

namespace Campus_live_rewrite.Controllers
{
    public class CampusNetController : Controller
    {
        [Route("/campus_net/online_players")]
        public ContentResult online_players()
        {
            return new ContentResult
            {
                Content = System.IO.File.ReadAllText("Web\\Campus_Net\\OnlinePlayers.html"),
                ContentType = "text/html"
            };
        }

        [Route("/campus_net/home")]
        public ContentResult CampusNetHome()
        {
            try
            {
                string authtoken = HttpContext.Request.Headers["Authorization"].ToString();

                if (string.IsNullOrEmpty(authtoken))
                {
                    authtoken = HttpContext.Request.Cookies["Authorization"];
                }

                if (string.IsNullOrEmpty(authtoken))
                {
                    Redirect("/campus_net/login");
                }

                string playerdata = ClientSecurity.DecodeToken(authtoken);

                if (playerdata == null)
                    Redirect("/campus_net/login");
                if (playerdata.Contains("error_token"))
                    Redirect("/campus_net/login");

                player_info? account = JsonConvert.DeserializeObject<player_info>(playerdata);

                return new ContentResult
                {
                    Content = System.IO.File.ReadAllText("Web\\Campus_Net\\Home.html"),
                    ContentType = "text/html"
                };
            }
            catch
            {
                return new ContentResult
                {
                    Content = System.IO.File.ReadAllText("Web\\Campus_Net\\Login.html"),
                    ContentType = "text/html"
                };
            }
        }

        [Route("/campus_net/login")]
        public ContentResult login()
        {
            return new ContentResult
            {
                Content = System.IO.File.ReadAllText("Web\\Campus_Net\\Login.html"),
                ContentType = "text/html"
            };
        }

        [Route("/campus_net/logout")]
        public ContentResult logout()
        {
            Response.Cookies.Append("Authorization", "", new Microsoft.AspNetCore.Http.CookieOptions
            {
                Expires = DateTime.Now,
                HttpOnly = true,
                Secure = true,
                SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Strict
            });
            return new ContentResult
            {
                Content = System.IO.File.ReadAllText("Web\\Campus_Net\\Login.html"),
                ContentType = "text/html"
            };
        }


        /*[HttpPost("/api/login")]
        public ContentResult Login()
        {
            try
            {
                string? username = Request.Form["username"];
                string? password = Request.Form["password"];


                if (string.IsNullOrWhiteSpace(username) || string.IsNullOrWhiteSpace(password))
                {
                    return new ContentResult
                    {
                        Content = "Your username or password is empty, please enter a username and password",
                        ContentType = "text/html"
                    };
                }


                var loginResult = PlayerDB.GetAccountPassword(username, password);

                if (loginResult == null)
                {
                    return new ContentResult
                    {
                        Content = "Invalid password or username, please use your @ name to login.",
                        ContentType = "text/html"
                    };
                }

                account_login<account_data> temp = PlayerDB.GetAccountPassword(username, password);

                string token = ClientSecurity.RefreshToken_Web(temp.Value.accountId);

                Response.Cookies.Append("Authorization", token, new Microsoft.AspNetCore.Http.CookieOptions
                {
                    Expires = DateTime.Now.AddDays(1),
                    HttpOnly = true,
                    Secure = true,
                    SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Strict
                });

                Response.Redirect($"/campus_net/home");

                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(temp),
                    ContentType = "text/html"
                };
            }
            catch
            {
                return new ContentResult
                {
                    Content = "Invalid username or password. Please try again.",
                    ContentType = "text/html"
                };
            }
        }*/

        [Route("/campus_net/account/me")]
        public IActionResult AccountMe()
        {
            string path = Path.Combine("Web", "Campus_Net", "Account.html");
            if (!System.IO.File.Exists(path))
                return NotFound("Account.html missing");
            return new ContentResult
            {
                Content = System.IO.File.ReadAllText(path),
                ContentType = "text/html"
            };
        }

        [HttpGet("/campus_net/account/me/data")]
        public IActionResult AccountMeData()
        {
            string authtoken = HttpContext.Request.Headers["Authorization"].ToString();
            if (string.IsNullOrEmpty(authtoken))
                authtoken = HttpContext.Request.Cookies["Authorization"];

            if (string.IsNullOrEmpty(authtoken))
                return Unauthorized("no_token");

            string decoded = ClientSecurity.DecodeToken(authtoken);
            if (string.IsNullOrEmpty(decoded) || decoded.StartsWith("error_token"))
                return Unauthorized(decoded);

            var info = JsonConvert.DeserializeObject<player_info>(decoded);
            if (info == null)
                return Unauthorized("decode_failed");

            var full = PlayerDB.GetFullAccount(info.Id);

            var payload = new
            {
                accountId   = info.Id,
                username    = info.Username,
                displayName = info.DisplayName,
                profileImage= info.profileImage,
                level       = info.Level,
                xp          = info.XP,
                createdAt   = info.created_at,
                bio         = full?.player_Extra?.Bio ?? "",
                discordId   = full?.player_Extra?.DiscordId.ToString() ?? "0",
                dormRoomId  = full?.player_Extra?.DormRoomID ?? 1UL
            };

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(payload),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        [HttpGet("/api/images/v3/feed/global")]
        public ContentResult GetGlobalFeedImages()
        {
            var images = ImageMetadataDB.GetGlobalImages();
            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(images),
                ContentType = "application/json",
                StatusCode = 200
            };
        }
    }
}
