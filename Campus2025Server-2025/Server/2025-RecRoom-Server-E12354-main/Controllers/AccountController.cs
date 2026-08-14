using Microsoft.AspNetCore.Mvc;
using Newtonsoft.Json;
using Campus_live_rewrite.api.dbs;
using Campus_live_rewrite.api.server;
using Campus_live_rewrite.api.server.Classes;
using Campus_live_rewrite.api.server.Classes.db_class;

namespace Campus_live_rewrite.Controllers
{
    [ApiController]
    [Route("/")]
    public class AccountController : ControllerBase
    {
        [HttpGet("acc/account/bulk")]
        public IActionResult GetAccountsBulk([FromQuery] List<ulong> id)
        {
            return Ok(PlayerDB.GetAccountsBulk(id));
        }

        // Self-service username set, used during first-run account creation
        // onboarding. Flips HasChosenUsername so the client's onboarding flow
        // considers this step done.
        [HttpPut("acc/account/me/username")]
        public ContentResult PutMyUsername([FromForm] string username)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            if (string.IsNullOrWhiteSpace(username))
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Username cannot be empty!",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            username = username.Trim();

            var existingPlayer = PlayerDB.GetFullAccountByName(username);
            if (existingPlayer != null && existingPlayer.playerid != account.Id)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "That username is already taken!",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            var player = PlayerDB.GetFullAccount(account.Id);
            if (player == null)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Player not found",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            player.player.Username = username;
            player.player.DisplayName = username;
            var col = PlayerDB.PlayerDBFile.GetCollection<player_data>("players");
            col.Update(player);

            // Set the account creation flag
            SettingsDB.SetPlayerSetting("Recroom.AccountCreation.HasChosenUsername", "True", account.Id);

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        // Self-service password set for onboarding. Password is hashed
        // before storage — never stored in plaintext.
        [HttpPut("acc/account/me/password")]
        public ContentResult PutMyPassword([FromForm] string password)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            if (string.IsNullOrWhiteSpace(password) || password.Length < 4)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Password must be at least 4 characters!",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            var player = PlayerDB.GetFullAccount(account.Id);
            if (player == null)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Player not found",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            player.password = ClientSecurity.HashPassword(password);
            var col = PlayerDB.PlayerDBFile.GetCollection<player_data>("players");
            col.Update(player);

            // Set the account creation flag
            SettingsDB.SetPlayerSetting("Recroom.AccountCreation.HasCreatedPassword", "True", account.Id);

            // Check if username has also been set
            var hasUsername = SettingsDB.GetPlayerSettingValue("Recroom.AccountCreation.HasChosenUsername", account.Id) == "True";
            if (hasUsername)
            {
                SettingsDB.SetPlayerSetting("Recroom.AccountCreation.HasFinished", "True", account.Id);
            }

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        [HttpPut("acc/account/me/profileimage")]
        public ContentResult PutMyProfileImage([FromForm] string imageName)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            if (string.IsNullOrWhiteSpace(imageName))
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Image name cannot be empty!",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            var player = PlayerDB.GetFullAccount(account.Id);
            if (player == null)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Player not found",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            player.player.profileImage = imageName;
            var col = PlayerDB.PlayerDBFile.GetCollection<player_data>("players");
            col.Update(player);

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        [HttpPut("acc/account/me/bannerimage")]
        public ContentResult PutMyBannerImage([FromForm] string imageName)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            if (string.IsNullOrWhiteSpace(imageName))
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Image name cannot be empty!",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            var player = PlayerDB.GetFullAccount(account.Id);
            if (player == null)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Player not found",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            player.player.bannerImage = imageName;
            var col = PlayerDB.PlayerDBFile.GetCollection<player_data>("players");
            col.Update(player);

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        [HttpPut("acc/account/me/bio")]
        public ContentResult PutMyBio([FromForm] string? bio)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            bio = System.Net.WebUtility.UrlDecode(bio ?? "");
            if (bio.Length > 255)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Your bio must not exceed 255 characters!",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            var player = PlayerDB.GetFullAccount(account.Id);
            if (player == null)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Player not found",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            player.player_Extra.Bio = bio;
            var col = PlayerDB.PlayerDBFile.GetCollection<player_data>("players");
            col.Update(player);

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        [HttpPut("acc/account/me/personalpronouns")]
        public ContentResult PutMyPronouns([FromForm] int pronounFlags)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            var player = PlayerDB.GetFullAccount(account.Id);
            if (player == null)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Player not found",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            player.player.pronounFlags = pronounFlags;
            var col = PlayerDB.PlayerDBFile.GetCollection<player_data>("players");
            col.Update(player);

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        [HttpPut("acc/account/me/identityflags")]
        public ContentResult PutMyIdentityFlags([FromForm] int identityFlags)
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            var player = PlayerDB.GetFullAccount(account.Id);
            if (player == null)
            {
                return new ContentResult
                {
                    Content = JsonConvert.SerializeObject(new
                    {
                        success = false,
                        error = "Player not found",
                        value = (string?)null
                    }),
                    ContentType = "application/json",
                    StatusCode = 200
                };
            }

            player.player.identityFlags = identityFlags;
            var col = PlayerDB.PlayerDBFile.GetCollection<player_data>("players");
            col.Update(player);

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        [HttpGet("acc/account/{accountId}/bio")]
        public IActionResult GetAccountBio(ulong accountId)
        {
            var player = PlayerDB.GetFullAccount(accountId);

            return Ok(new
            {
                AccountId = accountId,
                Bio = player?.player_Extra?.Bio ?? ""
            });
        }

        [HttpGet("acc/account/me")]
        public IActionResult GetAccountMe()
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            var accountData = PlayerDB.GetAccountMeSafe(account.Id);
            return accountData != null ? Ok(accountData) : NotFound();
        }

        [HttpPost("acc/account/me/email")]
        public IActionResult AccAccountMeEmail()
        {
            return Ok(new
            {
                success = true
            });
        }

        [HttpPost("acc/account/me/skip-orientation")]
        public ContentResult SkipOrientation()
        {
            var (account, error) = AuthHelper.ValidateToken(HttpContext);
            if (error != null)
                return error;

            // Set tutorial as complete to skip orientation
            SettingsDB.SetPlayerSetting("TUTORIAL_COMPLETE_MASK", "4294967295", account.Id);

            return new ContentResult
            {
                Content = JsonConvert.SerializeObject(new
                {
                    success = true,
                    error = "",
                    value = (string?)null
                }),
                ContentType = "application/json",
                StatusCode = 200
            };
        }

        // The client derives role-badge checks from the service map's Auth base,
        // so it calls /auth/role/... — keep both paths registered.
        [HttpGet("role/moderator/{playerId}")]
        [HttpGet("/auth/role/moderator/{playerId}")]
        public IActionResult CheckModerator(ulong playerId)
        {
            var player = PlayerDB.GetFullAccount(playerId);
            if (player == null)
                return NotFound();

            var isModerator = PlayerDB.CheckModFlag(playerId);
            return Ok(isModerator.ToString().ToLower());
        }

        [HttpGet("role/developer/{playerId}")]
        [HttpGet("/auth/role/developer/{playerId}")]
        public IActionResult CheckDeveloper(ulong playerId)
        {
            var player = PlayerDB.GetFullAccount(playerId);
            if (player == null)
                return NotFound();

            var isDeveloper = PlayerDB.CheckDevFlag(playerId);
            return Ok(isDeveloper.ToString().ToLower());
        }
    }
}
