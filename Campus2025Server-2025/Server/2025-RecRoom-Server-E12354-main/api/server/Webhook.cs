using Newtonsoft.Json;
using Campus_live_rewrite.api.dbs;
using Campus_live_rewrite.api.server.Classes.db_class;
using System.Text;

namespace Campus_live_rewrite.api.server
{
    public class Webhook
    {
        private static readonly Random rng = new Random();

        public static void Send(ulong accid, string type, string title, string description, long color, string? imageUrl = null, ulong? roomid = null)
        {
            string? webhookUrl = type.ToLower() switch
            {
                "prod" => "https://discord.com/api/webhooks/1537213168827498549/MF_rxl87Y_6QhNvGRFu5sZYA0jCE29n6LnzRn5pQJbfM0qUkyx1PfsWqQDfQ9lX-CuID",
                "admin" => "https://discord.com/api/webhooks/1537213524386779289/w4RpodUqFSQrCy_2EWqzeGA3bxYCO-kkjYrE9e-1kiaDeTFMlXjrRu-M8m-pCVqoLHcv",
                "roomie" => "https://discord.com/api/webhooks/1528472618649391184/sW9WSN-NqbWB1sj5lKdaUW4qKZogo_jDJ6ZJ3brpZGCMf3YwUpo7n2U3BeqyH1A5MCDO",
                "gaylog" => "https://discord.com/api/webhooks/1537213521685913601/oFiFq1IJZjqMkKaUV2_JZk16HLyMlXTVWVz8QeZ5N9dsrI2t7AobQj6wm-wchZYDO-Kt",
                _ => null
            };

            if (string.IsNullOrEmpty(webhookUrl))
            {
                Console.WriteLine($"Invalid webhook type: {type}");
                return;
            }


            player_data account = PlayerDB.GetFullAccount(accid);
            bool dev = PlayerDB.CheckDevFlag(accid);
            RoomDBClasses.RoomRoot? room = roomid.HasValue ? RoomDB.GetRoom(roomid.Value) : null;

            string authorName = account?.player?.Username ?? "Unknown User";
            if (dev)
            {
                //authorName += " [DEV]";
            }
            //string authorIcon = "https://example.com/icon.png";

            var payload = JsonConvert.SerializeObject(new discord_content
            {
                content = "",
                attachments = new List<string>(),
                embeds = new List<discord_embed>
                {
                    new discord_embed
                    {
                        title = title,
                        description = description,
                        color = (int)2067276,
                        thumbnail = room != null && !string.IsNullOrEmpty(room.ImageName) ? new { url = $"https://gameauth.ddns.net/img/{room.ImageName}" } : null,
                        image = string.IsNullOrEmpty(imageUrl) ? null : new discord_embed_image { url = imageUrl },
                        author = new discord_embed_author
                        {
                            name = authorName,
                            icon_url = $"https://gameauth.ddns.net/img/{account.player.profileImage}?width=80&cropSquare=true",
                            url = $"https://gameauth.ddns.net/campus_net/account/user/{account.player.Username}"
                        }
                    }
                }
            });

            Console.WriteLine(
                $"--- {title} ({type.ToUpper()}) ----------------------\n\n" +
                $"{description}\n\n" +
                $"----------------------------------");
            ulong number = (ulong)rng.Next(1000, 999999999);
            try
            {
                using var client = new HttpClient();
                var content = new StringContent(payload, Encoding.UTF8, "application/json");
                var response = client.PostAsync(webhookUrl, content).Result;

                if (!response.IsSuccessStatusCode)
                {
                    Console.WriteLine($"Failed to send webhook: {response.StatusCode} - {response.ReasonPhrase}");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error sending webhook: {ex.Message}");
            }
        }

        public class discord_content
        {
            public string? content { get; set; }
            public List<discord_embed>? embeds { get; set; }
            public List<string>? attachments { get; set; }
        }

        public class discord_embed
        {
            public string? title { get; set; }
            public string? description { get; set; }
            public long color { get; set; }
            public object? thumbnail { get; set; }
            public discord_embed_image? image { get; set; }
            public discord_embed_author? author { get; set; }
        }

        public class discord_embed_image
        {
            public string? url { get; set; }
        }
        public class discord_embed_author
        {
            public string? name { get; set; }
            public string? url { get; set; } 
            public string? icon_url { get; set; } 
        }
    }
}
