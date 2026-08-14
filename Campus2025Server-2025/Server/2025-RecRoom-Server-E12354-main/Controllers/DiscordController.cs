using Microsoft.AspNetCore.Mvc;
using Campus_live_rewrite.api.dbs;
using Campus_live_rewrite.api.server;
using System.Net.Http.Headers;
using System.Text.Json;

namespace Campus_live_rewrite.Controllers
{
    [ApiController]
    [Route("api/auth")]
    public class DiscordController : Controller
    {
        private const string ClientId = "1537203658725265488";
        private const string ClientSecret = "6JOoRs9-b-B8m8y5t9TnhURs6dOO-P-2";
        private const string RedirectUri = "https://gameauth.ddns.net/api/auth/discord/callback";

        [HttpGet("discord")]
        public IActionResult DiscordLogin()
        {
            var url =
                "https://discord.com/api/oauth2/authorize" +
                $"?client_id={ClientId}" +
                $"&redirect_uri={Uri.EscapeDataString(RedirectUri)}" +
                "&response_type=code" +
                "&scope=identify email";

            return Redirect(url);
        }

        [HttpGet("discord/callback")]
        public async Task<IActionResult> DiscordCallback([FromQuery] string code, [FromQuery] string? state = null)
        {
            if (string.IsNullOrEmpty(code))
                return BadRequest("Missing OAuth code.");

            using var http = new HttpClient();

            // Exchange code for access token
            var tokenResponse = await http.PostAsync(
                "https://discord.com/api/oauth2/token",
                new FormUrlEncodedContent(new Dictionary<string, string>
                {
                    ["client_id"] = ClientId,
                    ["client_secret"] = ClientSecret,
                    ["grant_type"] = "authorization_code",
                    ["code"] = code,
                    ["redirect_uri"] = RedirectUri
                })
            );

            if (!tokenResponse.IsSuccessStatusCode)
                return Unauthorized("OAuth token exchange failed.");

            var tokenJson = await tokenResponse.Content.ReadAsStringAsync();
            var token = JsonDocument.Parse(tokenJson)
                .RootElement.GetProperty("access_token").GetString();

            http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);

            var userJson = await http.GetStringAsync("https://discord.com/api/users/@me");
            var user = JsonDocument.Parse(userJson).RootElement;

            string discordId = user.GetProperty("id").GetString();
            string username = user.GetProperty("username").GetString();

            // Email may be null
            string? email = null;
            if (user.TryGetProperty("email", out var emailProp))
                email = emailProp.GetString();

            Console.WriteLine($"Discord Login: {username} ({discordId}) | Email: {email ?? "N/A"}");

            // 🔐 Lookup account (auto-create if this Discord id has never been seen)
            ulong discordIdUlong = ulong.Parse(discordId);
            var accountIds = PlayerDB.GetAccountIdsByDiscordId(discordIdUlong);

            ulong resolvedAccId;
            if (accountIds == null || !accountIds.Any())
            {
                Console.WriteLine($"[DiscordAuth] No account linked to discord_id={discordIdUlong} ({username}) — auto-creating.");
                if (!PlayerDB.CreateAccount(Campus_live_rewrite.api.server.Classes.db_class.Platforms.RecNet, 0UL, "discord-oauth", discordIdUlong, out var newAcc))
                {
                    Console.WriteLine($"[DiscordAuth] CreateAccount FAILED for discord_id={discordIdUlong}");
                    return StatusCode(500, "Failed to create Discord-linked account.");
                }
                Console.WriteLine($"[DiscordAuth] Auto-created account_id={newAcc.accountId} for discord_id={discordIdUlong}");
                resolvedAccId = newAcc.accountId;
            }
            else
            {
                resolvedAccId = accountIds.First();
                Console.WriteLine($"[DiscordAuth] Existing account_id={resolvedAccId} for discord_id={discordIdUlong}");
            }

            var account = PlayerDB.GetFullAccount(resolvedAccId);
            if (account is null)
            {
                Console.WriteLine($"[DiscordAuth] GetFullAccount returned null for accountId={resolvedAccId} (discord_id={discordIdUlong})");
                return Unauthorized("Account not found.");
            }

            // ✅ Issue JWT
            string jwtToken = ClientSecurity.RefreshToken_Web(account.playerid);

            // SameSite=Lax (not Strict): a cookie set on a cross-site redirect from Discord is
            // still allowed AND is sent on the follow-up top-level GET (the /admin/home/v4
            // redirect). With Strict, the browser refuses to send it on that navigation, so the
            // admin page bounces you right back to the login form — which was the symptom.
            Response.Cookies.Append("Authorization", jwtToken, new Microsoft.AspNetCore.Http.CookieOptions
            {
                Expires = DateTime.Now.AddHours(24),
                HttpOnly = true,
                Secure = true,
                SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Lax,
                Path = "/"
            });

            // Discriminate based on `state`. Web-admin login (default / "admin") stays on the
            // original path — cookie is set on gameauth.ddns.net and we redirect to the admin
            // dashboard. Launcher OAuth (state="launcher:<port>") skips the cookie and instead
            // redirects the browser to the launcher's local http listener with the JWT in the
            // query string; the launcher picks it up there and closes the browser tab.
            if (!string.IsNullOrEmpty(state) && state.StartsWith("launcher:", StringComparison.Ordinal))
            {
                string portStr = state.Substring("launcher:".Length);
                if (int.TryParse(portStr, out int port) && port >= 1024 && port <= 65535)
                {
                    string dest = $"http://127.0.0.1:{port}/campus-auth?token={Uri.EscapeDataString(jwtToken)}&account_id={account.playerid}";
                    Console.WriteLine($"[DiscordAuth] launcher flow — redirecting to {dest}");
                    return Redirect(dest);
                }
                Console.WriteLine($"[DiscordAuth] launcher flow — malformed port in state='{state}', falling through to admin redirect");
            }

            Console.WriteLine($"[DiscordAuth] Issued admin JWT for account_id={account.playerid}, redirecting to /admin/home/v4");
            return Redirect("/admin/home/v4");
        }
    }
}
