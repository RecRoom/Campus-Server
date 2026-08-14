using LiteDB;
using Microsoft.Extensions.FileProviders;
using Campus_live_rewrite;
using Campus_live_rewrite.api.dbs;
using Campus_live_rewrite.Controllers;
using Campus_live_rewrite.Commands;
using Campus_live_rewrite.api.server;
using System.Security.Principal;
using Microsoft.AspNetCore.Mvc;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Campus_live_rewrite.api.server.Classes;
using Newtonsoft.Json;

internal class Program
{
    private static async Task Main(string[] args)
    {
        var builder = WebApplication.CreateBuilder(args);

        builder.WebHost.ConfigureKestrel(serverOptions =>
        {
            serverOptions.AllowSynchronousIO = true;
        });

        builder.Services.AddHttpLogging(configureOptions => { });

        builder.Services
    .AddControllers()
    .AddJsonOptions(o =>
    {
        o.JsonSerializerOptions.PropertyNamingPolicy = null;
    });

        builder.Services.AddSignalR();

        builder.Services.AddSingleton<DiscordBot>();

        builder.Host.UseContentRoot(Directory.GetCurrentDirectory());

        builder.Services.Configure<ApiBehaviorOptions>(options =>
        {
            options.SuppressModelStateInvalidFilter = false;
        });

        var app = builder.Build();

        if (!Directory.Exists(Environment.CurrentDirectory + "/Data"))
        {
            Directory.CreateDirectory(Environment.CurrentDirectory + "/Data");
            Directory.CreateDirectory(Environment.CurrentDirectory + "/Data/APIS");
            Directory.CreateDirectory(Environment.CurrentDirectory + "/Data/DBs");
        }

        if (!Directory.Exists(Environment.CurrentDirectory + "/Web"))
        {
            Directory.CreateDirectory(Environment.CurrentDirectory + "/Web");
            Directory.CreateDirectory(Environment.CurrentDirectory + "/Web/Admin");
        }

        app.UseMiddleware<Log>(Path.Combine(Environment.CurrentDirectory, "Data", "APILog.txt"));

        app.UseMiddleware<RequestResponseLoggingMiddleware>();

        //app.UseMiddleware<VpnBlockMiddleware>();

        app.Use(async (context, next) =>
        {
            var path = context.Request.Path.ToString();
            if (path.Contains("//"))
            {
                path = path.Replace("//", "/");
                context.Request.Path = path;
            }
            await next.Invoke();
        });

        app.UseWebSockets();

        app.UseAuthorization();

        app.UseMiddleware<IPAllowMiddleware>();

        // app.UseStatusCodePagesWithReExecute("/Web/404");

        var webSocketOptions = new WebSocketOptions
        {
            KeepAliveInterval = TimeSpan.FromMinutes(2)
        };

        app.UseWebSockets(webSocketOptions);

        app.UseStaticFiles(new StaticFileOptions
        {
            FileProvider = new PhysicalFileProvider(Path.Combine(builder.Environment.ContentRootPath, "Data", "Videos")),
            RequestPath = "/cdn/video",
            OnPrepareResponse = ctx =>
            {
                ctx.Context.Response.Headers.Remove("Accept-Ranges");
                var allowedContentTypes = new[] 
                {
                    "video/mp4", // some webm video break recroom video screen and show artifact
                    "video/mkv"
                };
                if (!allowedContentTypes.Contains(ctx.Context.Response.ContentType))
                {
                    ctx.Context.Response.StatusCode = StatusCodes.Status403Forbidden;
                    ctx.Context.Response.Body = Stream.Null;
                }
            }
        });

        app.UseStaticFiles(new StaticFileOptions
        {
            FileProvider = new PhysicalFileProvider(
         Path.Combine(builder.Environment.ContentRootPath, "Data", "CDN", "Configs")
     ),
            RequestPath = "/cdn/config",
            OnPrepareResponse = ctx =>
            {
                ctx.Context.Response.Headers.Remove("Accept-Ranges");
            },
            ServeUnknownFileTypes = true // optional if some files have unknown extensions
        });

        app.UseHttpLogging();

        // Campus admin panel: the LoginV4.html / HomeV4.html templates reference
        // /admin-assets/Cutout.png, /admin-assets/Background.png, /admin-assets/AnimatedIcon.png
        // for the Campus branding (purple/orange gradient + tile pattern + Cutout hero).
        // Mount those files from ContentRoot/AdminPanel/Assets - PackageServer.ps1 ships that
        // folder into the deploy tarball alongside Data/ and Web/.
        {
            string adminAssets = Path.Combine(builder.Environment.ContentRootPath, "AdminPanel", "Assets");
            if (Directory.Exists(adminAssets))
            {
                app.UseStaticFiles(new StaticFileOptions
                {
                    FileProvider = new PhysicalFileProvider(Path.GetFullPath(adminAssets)),
                    RequestPath = "/admin-assets",
                });
                Console.WriteLine($"[admin] assets from {Path.GetFullPath(adminAssets)}");
            }
            else
            {
                Console.WriteLine($"[admin] WARN: AdminPanel/Assets not found at {adminAssets} - Campus branding won't load");
            }
        }

        app.MapControllers();

        //PlayerDB.EnsureDormsDontBeStupid(log: true);

        //RoomDB.d(51, 138, "f94c6b386d2b45a0a582f688022aba0c.room");

        //PlayerDB.ImportPlayers(Environment.CurrentDirectory + "/Data/exported_players.json");
        //await RoomDB.ImportRooms(Environment.CurrentDirectory + "/Data/yay.json");
        // Once everything is checked for prod release, uncomment this and import the players and rooms.
        /*
        await Task.Run(() =>
        {
            try
            {
                Webhook.Send(
                1,
                "prod",
                "Server Info",
                "Campus Live server just went online!",
                5898927
                );

            }
            catch (Exception ex)
            {
                Console.WriteLine($"{ex.Message}");
            }
        });
        */
        // PlayerDB.SetIsInfluencer(282, true);
        // PlayerDB.SetCreatorCode(282, "idk");
        // RoomDB.FixDuplicateDorms();
        // PlayerDB.EnsureDormsForAllPlayers();
        // PlayerDB.SetPlayerDev(Campus_live_rewrite.api.server.Classes.db_class.DevFlag.none, 3);
        // PlayerDB.ClearRecentRoomsForAllPlayers();
        // Start Discord bot in background — never let a bot fault take the web server down.
        _ = Task.Run(async () =>
        {
            try
            {
                Console.WriteLine("[DiscordBot] Starting...");
                await StartBotAsync(app.Services);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[DiscordBot] CRASHED: {ex}");
            }
        });
        // Only start the interactive command console when stdin is attached to a terminal.
        // Under systemd, stdin is /dev/null → ReadLine() returns null immediately and the loop
        // busy-spins, flooding the journal and starving other threads.
        if (!Console.IsInputRedirected)
        {
            new Thread(new ThreadStart(debug_run)).Start();
        }
        else
        {
            Console.WriteLine("[Startup] stdin is redirected — skipping CommandConsole (headless mode).");
        }
        
        PlayerDB.Setup();
        HeartbeatDB.Setup();
        RoomDB.Setup();

        // ---- Admin seed --------------------------------------------------------------------
        // Idempotent. On every start, make sure there's a Kyle account with dev_Flag=dev and
        // the operator password so the admin panel is always reachable, even after a fresh
        // deploy that ships with an empty Players.db. If the account already exists we DON'T
        // clobber it - operator may have changed the password from the panel.
        try
        {
            var col = Campus_live_rewrite.api.dbs.PlayerDB.PlayerDBFile
                        .GetCollection<Campus_live_rewrite.api.server.Classes.db_class.player_data>("players");
            var existing = col.FindOne(x => x.player != null && x.player.Username == "Kyle");
            if (existing == null)
            {
                // Seed via CreateAccount - random name + starter reputation - then rename to Kyle.
                if (Campus_live_rewrite.api.dbs.PlayerDB.CreateAccount(
                        Campus_live_rewrite.api.server.Classes.db_class.Platforms.All,
                        0UL, "seed-admin", 0UL, out var seeded))
                {
                    var fresh = col.FindOne(x => x.playerid == seeded.accountId);
                    if (fresh != null && fresh.player != null)
                    {
                        fresh.player.Username = "Kyle";
                        fresh.player.DisplayName = "Kyle";
                        fresh.password = Campus_live_rewrite.api.server.ClientSecurity.HashPassword("VanessaPlease16#");
                        fresh.dev_Flag = Campus_live_rewrite.api.server.Classes.db_class.DevFlag.dev;
                        col.Update(fresh);
                        Console.WriteLine($"[admin-seed] created Kyle (id={fresh.playerid}) with dev flag - login: Kyle / VanessaPlease16#");
                    }
                }
                else
                {
                    Console.WriteLine("[admin-seed] CreateAccount returned false, Kyle NOT seeded");
                }
            }
            else
            {
                bool changed = false;
                if (string.IsNullOrEmpty(existing.password))
                {
                    existing.password = Campus_live_rewrite.api.server.ClientSecurity.HashPassword("VanessaPlease16#");
                    changed = true;
                }
                if ((existing.dev_Flag & Campus_live_rewrite.api.server.Classes.db_class.DevFlag.dev) !=
                     Campus_live_rewrite.api.server.Classes.db_class.DevFlag.dev)
                {
                    existing.dev_Flag |= Campus_live_rewrite.api.server.Classes.db_class.DevFlag.dev;
                    changed = true;
                }
                // Stamp Kyle's owner Discord + Steam IDs.
                existing.player_Extra ??= new Campus_live_rewrite.api.server.Classes.db_class.player_extra();
                if (existing.player_Extra.DiscordId != 1450391092984807445UL)
                {
                    existing.player_Extra.DiscordId = 1450391092984807445UL;
                    changed = true;
                }
                existing.platform = Campus_live_rewrite.api.server.Classes.db_class.Platforms.Steam;
                existing.player.PlatformIds ??= new List<Campus_live_rewrite.api.server.Classes.db_class.mPlatformID>();
                var steamEntry = existing.player.PlatformIds.FirstOrDefault(p => p != null && p.Platform == Campus_live_rewrite.api.server.Classes.db_class.Platforms.Steam);
                if (steamEntry == null)
                {
                    existing.player.PlatformIds.Add(new Campus_live_rewrite.api.server.Classes.db_class.mPlatformID
                    {
                        Platform = Campus_live_rewrite.api.server.Classes.db_class.Platforms.Steam,
                        PlatformId = 76561199059115358UL
                    });
                    changed = true;
                }
                else if (steamEntry.PlatformId != 76561199059115358UL)
                {
                    steamEntry.PlatformId = 76561199059115358UL;
                    changed = true;
                }
                if (changed) { col.Update(existing); Console.WriteLine($"[admin-seed] refreshed Kyle (id={existing.playerid}) - dev flag / discord / steam stamped"); }
                else         { Console.WriteLine($"[admin-seed] Kyle (id={existing.playerid}) already fine - no changes"); }

                // Force skip-orientation flags on Kyle so first login on this build doesn't
                // loop through the broken SocialOrientation load path.
                Campus_live_rewrite.api.dbs.SettingsDB.SetPlayerSetting("TUTORIAL_COMPLETE_MASK",              "4294967295", existing.playerid);
                Campus_live_rewrite.api.dbs.SettingsDB.SetPlayerSetting("Recroom.AccountCreation.HasFinished", "true",       existing.playerid);
                Campus_live_rewrite.api.dbs.SettingsDB.SetPlayerSetting("Recroom.AccountCreation.HasChosenUsername",  "true", existing.playerid);
                Campus_live_rewrite.api.dbs.SettingsDB.SetPlayerSetting("Recroom.AccountCreation.HasCreatedPassword", "true", existing.playerid);
            }
        }
        catch (Exception seedEx)
        {
            Console.WriteLine($"[admin-seed] FAILED: {seedEx.Message}");
        }

        app.Run();
        //await botTask;
    }
    public static async Task StartBotAsync(IServiceProvider services)
    {
        var bot = services.GetRequiredService<DiscordBot>();
        await bot.RunAsync();
    }

    public static void debug_run()
    {
        new CommandConsole();
    }
}