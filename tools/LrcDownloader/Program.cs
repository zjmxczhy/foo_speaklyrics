using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using System.Xml;

namespace LrcDownloader
{
    internal static class Program
    {
        private const string BaseUrl = "https://lrclib.net";
        private const string BaseUrlQqMusic = "https://u.y.qq.com/cgi-bin/musicu.fcg";
        private const string UserAgent = "foo_speaklyrics-lrcdownloader/0.1 (.NET Framework 4.8)";

        private static int Main(string[] args)
        {
            Console.OutputEncoding = new UTF8Encoding(false);
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;

            try
            {
                var options = ParseArgs(args);
                if (options.Help)
                {
                    PrintUsage();
                    return 0;
                }

                if (string.IsNullOrWhiteSpace(options.Title))
                {
                    Console.Error.WriteLine("ERROR: --title is required.");
                    PrintUsage();
                    return 2;
                }

                if (options.ListOnly)
                {
                    PrintSearchResults(options);
                    return 0;
                }

                if (string.IsNullOrWhiteSpace(options.OutDir))
                {
                    Console.Error.WriteLine("ERROR: --out is required.");
                    PrintUsage();
                    return 2;
                }

                Directory.CreateDirectory(options.OutDir);

                var record = FindBestLyrics(options);
                if (record == null || string.IsNullOrWhiteSpace(record.SyncedLyrics))
                {
                    Console.Error.WriteLine("NOT_FOUND: synced LRC lyrics were not found.");
                    return 1;
                }

                var fileName = !string.IsNullOrWhiteSpace(options.FileName)
                    ? options.FileName
                    : BuildFileName(record.ArtistName, record.TrackName, options.Artist, options.Title);
                if (!fileName.EndsWith(".lrc", StringComparison.OrdinalIgnoreCase)) fileName += ".lrc";

                var outputPath = Path.Combine(options.OutDir, SanitizeFileName(fileName));
                File.WriteAllText(outputPath, NormalizeNewlines(StripEnhancedLrcTags(record.SyncedLyrics)), new UTF8Encoding(false));

                Console.WriteLine(outputPath);
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("ERROR: " + ex.Message);
                return 3;
            }
        }


        private static void PrintSearchResults(Options options)
        {
            var results = SearchAllSources(options);
            foreach (var item in results)
            {
                Console.WriteLine(SafeTsv(item.TrackName) + "\t" +
                                  SafeTsv(item.ArtistName) + "\t" +
                                  SafeTsv(item.SourceKey));
            }
        }

        private static List<LyricsRecord> SearchAllSources(Options options)
        {
            var results = new List<LyricsRecord>();
            if (IsSourceEnabled(options, "lrclib"))
            {
                TryAppendSearchResults(results, () => Search(options), "lrclib", "LRCLIB", options);
            }
            if (IsSourceEnabled(options, "qq1") || IsSourceEnabled(options, "qq"))
            {
                TryAppendSearchResults(results, () => SearchQqMusic(options), "qq1", "QQ\u97F3\u4E501\u53F7\u6E90", options);
            }
            if (IsSourceEnabled(options, "qq2") || IsSourceEnabled(options, "qq"))
            {
                TryAppendSearchResults(results, () => { var qq2Results = SearchQqMusicPcLrc(options); return qq2Results.Count > 0 ? qq2Results : SearchQqMusic(options); }, "qq2", "QQ\u97F3\u4E502\u53F7\u6E90", options);
            }
            if (IsSourceEnabled(options, "netease") || IsSourceEnabled(options, "163"))
            {
                TryAppendSearchResults(results, () => SearchNeteaseMusic(options), "netease", "\u7F51\u6613\u4E91\u97F3\u4E50", options);
            }
            results.Sort((left, right) => Score(right, options).CompareTo(Score(left, options)));
            return DeduplicateResults(results);
        }

        private static void TryAppendSearchResults(List<LyricsRecord> target, Func<List<LyricsRecord>> search, string sourceKey, string sourceName, Options options)
        {
            try
            {
                foreach (var item in search())
                {
                    if (item == null || string.IsNullOrWhiteSpace(item.TrackName)) continue;
                    item.SourceKey = sourceKey;
                    item.SourceDisplayName = sourceName;
                    item.ScoreValue = Score(item, options);
                    target.Add(item);
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("WARN: " + sourceName + " search failed: " + ex.Message);
            }
        }

        private static List<LyricsRecord> DeduplicateResults(List<LyricsRecord> input)
        {
            var output = new List<LyricsRecord>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var item in input)
            {
                var key = NormalizeForMatch(item.TrackName) + "|" + NormalizeForMatch(item.ArtistName) + "|" + item.SourceKey;
                if (seen.Add(key)) output.Add(item);
            }
            return output;
        }

        private static string SafeTsv(string value)
        {
            return (value ?? string.Empty).Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ').Trim();
        }

        private static LyricsRecord FindBestLyrics(Options options)
        {
            var lrclibEnabled = IsSourceEnabled(options, "lrclib");
            var neteaseEnabled = IsSourceEnabled(options, "netease") || IsSourceEnabled(options, "163");
            var qq1Enabled = IsSourceEnabled(options, "qq1") || IsSourceEnabled(options, "qq");
            var qq2Enabled = IsSourceEnabled(options, "qq2") || IsSourceEnabled(options, "qq");
            if (!lrclibEnabled && !neteaseEnabled && !qq1Enabled && !qq2Enabled) return null;

            if (lrclibEnabled)
            {
                // Full signature lookup first. It is stricter and can return the best single match.
                if (!options.SearchOnly && !string.IsNullOrWhiteSpace(options.Artist) &&
                    !string.IsNullOrWhiteSpace(options.Album) && options.DurationSeconds > 0)
                {
                    var exact = TryGetBySignature(options, cached: true);
                    if (exact != null && !string.IsNullOrWhiteSpace(exact.SyncedLyrics)) return exact;

                    if (!options.CachedOnly)
                    {
                        exact = TryGetBySignature(options, cached: false);
                        if (exact != null && !string.IsNullOrWhiteSpace(exact.SyncedLyrics)) return exact;
                    }
                }

                var results = Search(options);
                LyricsRecord best = null;
                var bestScore = int.MinValue;
                foreach (var item in results)
                {
                    if (string.IsNullOrWhiteSpace(item.SyncedLyrics)) continue;
                    var score = Score(item, options);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = item;
                    }
                }
                if (best != null) return best;
            }

            if (qq1Enabled)
            {
                var qq1 = TryGetFromQqMusicLrcSource(options);
                if (qq1 != null && !string.IsNullOrWhiteSpace(qq1.SyncedLyrics)) return qq1;
            }

            if (qq2Enabled)
            {
                var qq2 = TryGetFromQqMusicQrcSource(options);
                if (qq2 != null && !string.IsNullOrWhiteSpace(qq2.SyncedLyrics)) return qq2;
            }

            if (neteaseEnabled)
            {
                var netease = TryGetFromNeteaseMusic(options);
                if (netease != null && !string.IsNullOrWhiteSpace(netease.SyncedLyrics)) return netease;
            }

            return null;
        }

        private static LyricsRecord TryGetBySignature(Options options, bool cached)
        {
            var path = cached ? "/api/get-cached" : "/api/get";
            var url = BaseUrl + path + "?track_name=" + Url(options.Title) +
                      "&artist_name=" + Url(options.Artist) +
                      "&album_name=" + Url(options.Album) +
                      "&duration=" + options.DurationSeconds.ToString(CultureInfo.InvariantCulture);
            try
            {
                var json = HttpGet(url);
                var obj = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
                return LyricsRecord.FromDictionary(obj);
            }
            catch (WebException ex)
            {
                var response = ex.Response as HttpWebResponse;
                if (response != null && response.StatusCode == HttpStatusCode.NotFound) return null;
                throw;
            }
        }

        private static List<LyricsRecord> Search(Options options)
        {
            string url;
            if (!string.IsNullOrWhiteSpace(options.Artist))
            {
                url = BaseUrl + "/api/search?track_name=" + Url(options.Title) + "&artist_name=" + Url(options.Artist);
                if (!string.IsNullOrWhiteSpace(options.Album)) url += "&album_name=" + Url(options.Album);
            }
            else
            {
                url = BaseUrl + "/api/search?q=" + Url(options.Title);
            }

            var json = HttpGet(url);
            var arr = new JavaScriptSerializer().DeserializeObject(json) as object[];
            var list = new List<LyricsRecord>();
            if (arr == null) return list;
            foreach (var item in arr)
            {
                var dict = item as Dictionary<string, object>;
                var rec = LyricsRecord.FromDictionary(dict);
                if (rec != null) list.Add(rec);
            }
            return list;
        }


        private static LyricsRecord TryGetFromQqMusicLrcSource(Options options)
        {
            var candidates = SearchQqMusic(options);
            LyricsRecord best = null;
            var bestScore = int.MinValue;
            foreach (var item in candidates)
            {
                var score = Score(item, options);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = item;
                }
            }

            if (best == null) return null;

            if (!string.IsNullOrWhiteSpace(best.SourceId)) best.SyncedLyrics = GetQqMusicLyric(best.SourceId);
            if (string.IsNullOrWhiteSpace(best.SyncedLyrics)) best.SyncedLyrics = GetQqMusicLyricDownload(best);
            return string.IsNullOrWhiteSpace(best.SyncedLyrics) ? null : best;
        }

        private static LyricsRecord TryGetFromQqMusicQrcSource(Options options)
        {
            var candidates = SearchQqMusicPcLrc(options);
            if (candidates.Count == 0) candidates = SearchQqMusic(options);
            candidates.Sort((left, right) => Score(right, options).CompareTo(Score(left, options)));

            foreach (var candidate in candidates)
            {
                candidate.SyncedLyrics = GetQqMusicLyricFromPlayLyricInfo(candidate, options);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics)) candidate.SyncedLyrics = GetQqMusicLyricDownload(candidate);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics) && !string.IsNullOrWhiteSpace(candidate.SourceId))
                {
                    candidate.SyncedLyrics = GetQqMusicLyric(candidate.SourceId);
                }
                if (!string.IsNullOrWhiteSpace(candidate.SyncedLyrics)) return candidate;
            }

            return null;
        }

        private static LyricsRecord TryGetFromNeteaseMusic(Options options)
        {
            var candidates = SearchNeteaseMusic(options);
            candidates.Sort((left, right) => Score(right, options).CompareTo(Score(left, options)));

            foreach (var candidate in candidates)
            {
                if (string.IsNullOrWhiteSpace(candidate.SourceId)) continue;
                candidate.SyncedLyrics = GetNeteaseMusicLyric(candidate.SourceId);
                if (!string.IsNullOrWhiteSpace(candidate.SyncedLyrics)) return candidate;
            }

            return null;
        }

        private static List<LyricsRecord> SearchNeteaseMusic(Options options)
        {
            var query = options.Title;
            if (!string.IsNullOrWhiteSpace(options.Artist)) query += " " + options.Artist;

            var data = new Dictionary<string, object>
            {
                { "s", ProcKeywords(query) },
                { "type", 1 },
                { "limit", 10 },
                { "offset", 0 }
            };

            var json = NeteaseLinuxApiPost("POST", "https://music.163.com/api/search/get", data);
            var root = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            var result = GetDict(root, "result");
            var songs = GetArray(result, "songs");

            var results = new List<LyricsRecord>();
            if (songs == null) return results;
            foreach (var entry in songs)
            {
                var song = entry as Dictionary<string, object>;
                if (song == null) continue;

                var id = GetString(song, "id");
                var title = DecodeNeteaseText(GetString(song, "name"));
                if (string.IsNullOrWhiteSpace(id) || string.IsNullOrWhiteSpace(title)) continue;

                var artists = GetArray(song, "artists");
                var album = GetDict(song, "album");
                results.Add(new LyricsRecord
                {
                    TrackName = title,
                    ArtistName = BuildNeteaseArtistText(artists),
                    AlbumName = DecodeNeteaseText(GetString(album, "name")),
                    DurationSeconds = NeteaseDurationToSeconds(GetInt(song, "duration")),
                    SourceId = id
                });
            }
            return results;
        }

        private static string GetNeteaseMusicLyric(string songId)
        {
            var data = new Dictionary<string, object> { { "id", songId } };
            var json = NeteaseLinuxApiPost("POST", "https://music.163.com/api/song/lyric?lv=-1&kv=-1&tv=-1", data);
            var root = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;

            var lrc = GetDict(root, "lrc");
            var lyric = CleanNeteaseLyric(GetString(lrc, "lyric"));
            if (IsUsableLrc(lyric)) return lyric;

            var tlyric = GetDict(root, "tlyric");
            lyric = CleanNeteaseLyric(GetString(tlyric, "lyric"));
            return IsUsableLrc(lyric) ? lyric : string.Empty;
        }

        private static List<LyricsRecord> SearchQqMusic(Options options)
        {
            var query = options.Title;
            if (!string.IsNullOrWhiteSpace(options.Artist)) query += " " + options.Artist;

            var payload = new Dictionary<string, object>
            {
                {
                    "req", new Dictionary<string, object>
                    {
                        { "method", "DoSearchForQQMusicDesktop" },
                        { "module", "music.search.SearchCgiService" },
                        {
                            "param", new Dictionary<string, object>
                            {
                                { "query", query },
                                { "num_per_page", 10 },
                                { "page_num", 1 },
                                { "search_type", 0 }
                            }
                        }
                    }
                }
            };

            var json = HttpPostJson(BaseUrlQqMusic, new JavaScriptSerializer().Serialize(payload));
            var root = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            var req = GetDict(root, "req");
            var data = GetDict(req, "data");
            var body = GetDict(data, "body");
            var song = GetDict(body, "song");
            var list = GetArray(song, "list");

            var results = new List<LyricsRecord>();
            if (list == null) return results;
            foreach (var entry in list)
            {
                var item = entry as Dictionary<string, object>;
                if (item == null) continue;

                var album = GetDict(item, "album");
                var singers = GetArray(item, "singer");
                var artist = BuildSingerText(singers);
                var title = FirstNonEmpty(GetString(item, "name"), GetString(item, "title"), GetString(item, "songname"));
                var songMid = FirstNonEmpty(GetString(item, "mid"), GetString(item, "songmid"));
                if (string.IsNullOrWhiteSpace(title) || string.IsNullOrWhiteSpace(songMid)) continue;

                results.Add(new LyricsRecord
                {
                    TrackName = title,
                    ArtistName = artist,
                    AlbumName = FirstNonEmpty(GetString(album, "name"), GetString(item, "albumname")),
                    DurationSeconds = GetInt(item, "interval"),
                    SourceId = songMid,
                    SourceNumericId = GetInt(item, "id")
                });
            }
            return results;
        }


        private static List<LyricsRecord> SearchQqMusicPcLrc(Options options)
        {
            var url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_search_pc_lrc.fcg?SONGNAME=" + Url(options.Title) +
                      "&SINGERNAME=" + Url(options.Artist) +
                      "&TYPE=2&RANGE_MIN=1&RANGE_MAX=20";
            var body = HttpGet(url);
            var results = new List<LyricsRecord>();
            try
            {
                var doc = new XmlDocument { XmlResolver = null };
                doc.LoadXml(body);
                var songInfo = doc.GetElementsByTagName("songinfo");
                var defaultTitle = DecodePercent(GetFirstCData(doc, "songname"));
                var defaultArtist = DecodePercent(GetFirstCData(doc, "singer"));
                foreach (XmlNode song in songInfo)
                {
                    var idText = song.Attributes != null && song.Attributes["id"] != null ? song.Attributes["id"].Value : string.Empty;
                    int songId;
                    if (!int.TryParse(idText, NumberStyles.Integer, CultureInfo.InvariantCulture, out songId) || songId <= 0) continue;
                    var title = DecodePercent(GetChildText(song, "name"));
                    var artist = DecodePercent(GetChildText(song, "singername"));
                    var album = DecodePercent(GetChildText(song, "albumname"));
                    if (string.IsNullOrWhiteSpace(title)) title = defaultTitle;
                    if (string.IsNullOrWhiteSpace(artist)) artist = defaultArtist;
                    if (string.IsNullOrWhiteSpace(title)) continue;

                    results.Add(new LyricsRecord
                    {
                        TrackName = title,
                        ArtistName = artist,
                        AlbumName = album,
                        DurationSeconds = 0,
                        SourceNumericId = songId
                    });
                }
            }
            catch
            {
                return results;
            }
            return results;
        }


        private static string GetQqMusicLyricFromPlayLyricInfo(LyricsRecord record, Options options)
        {
            if (record.SourceNumericId <= 0) return string.Empty;

            var payload = new Dictionary<string, object>
            {
                {
                    "comm", new Dictionary<string, object>
                    {
                        { "_channelid", "0" },
                        { "_os_version", "6.2.9200-2" },
                        { "authst", string.Empty },
                        { "ct", "19" },
                        { "cv", "1873" },
                        { "patch", "118" },
                        { "psrf_access_token_expiresAt", 0 },
                        { "psrf_qqaccess_token", string.Empty },
                        { "psrf_qqopenid", string.Empty },
                        { "psrf_qqunionid", string.Empty },
                        { "tmeAppID", "qqmusic" },
                        { "tmeLoginType", 2 },
                        { "uin", "0" },
                        { "wid", "0" }
                    }
                },
                {
                    "music.musichallSong.PlayLyricInfo.GetPlayLyricInfo", new Dictionary<string, object>
                    {
                        { "method", "GetPlayLyricInfo" },
                        { "module", "music.musichallSong.PlayLyricInfo" },
                        {
                            "param", new Dictionary<string, object>
                            {
                                { "albumName", Base64Utf8(record.AlbumName) },
                                { "crypt", 1 },
                                { "ct", 19 },
                                { "cv", 1873 },
                                { "interval", options.DurationSeconds > 0 ? options.DurationSeconds : record.DurationSeconds },
                                { "lrc_t", 0 },
                                { "qrc", 1 },
                                { "qrc_t", 0 },
                                { "roma", 0 },
                                { "roma_t", 0 },
                                { "singerName", Base64Utf8(record.ArtistName) },
                                { "songID", record.SourceNumericId },
                                { "songName", Base64Utf8(record.TrackName) },
                                { "trans", 0 },
                                { "trans_t", 0 },
                                { "type", -1 }
                            }
                        }
                    }
                }
            };

            var url = BaseUrlQqMusic + "?pcachetime=" + CurrentUnixMilliseconds().ToString(CultureInfo.InvariantCulture);
            var json = HttpPostJson(url, new JavaScriptSerializer().Serialize(payload));
            var root = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            var lyricRoot = GetDict(root, "music.musichallSong.PlayLyricInfo.GetPlayLyricInfo");
            if (GetInt(lyricRoot, "code") != 0) return string.Empty;

            var data = GetDict(lyricRoot, "data");
            if (data == null || GetInt(data, "songID") != record.SourceNumericId) return string.Empty;

            return FirstUsableLyric(
                GetString(data, "lyric"),
                GetString(data, "qrc"),
                GetString(data, "trans"));
        }

        private static string GetQqMusicLyricDownload(LyricsRecord record)
        {
            if (record.SourceNumericId <= 0) return string.Empty;

            var url = "https://c.y.qq.com/qqmusic/fcgi-bin/lyric_download.fcg?version=15&miniversion=82&lrctype=4&musicid=" +
                      record.SourceNumericId.ToString(CultureInfo.InvariantCulture);
            var body = HttpGet(url);
            body = body.Replace("<!--", string.Empty).Replace("-->", string.Empty);
            body = Regex.Replace(body, @"<miniversion[^>]*/>", string.Empty).Trim();

            try
            {
                var doc = new XmlDocument { XmlResolver = null };
                doc.LoadXml(body);
                var nodes = doc.GetElementsByTagName("content");
                foreach (XmlNode node in nodes)
                {
                    var lyric = ConvertPossibleQqLyricToLrc(node.InnerText);
                    if (IsUsableLrc(lyric)) return lyric;
                }
            }
            catch
            {
                return ConvertPossibleQqLyricToLrc(body);
            }

            return string.Empty;
        }

        private static string GetQqMusicLyric(string songMid)
        {
            var url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?nobase64=1&songmid=" +
                      Url(songMid) + "&format=json&inCharset=utf8&outCharset=utf-8";
            var json = HttpGet(url);
            var obj = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            var lyric = GetString(obj, "lyric");
            if (!string.IsNullOrWhiteSpace(lyric)) return lyric;

            url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=" +
                  Url(songMid) + "&format=json&inCharset=utf8&outCharset=utf-8";
            json = HttpGet(url);
            obj = new JavaScriptSerializer().DeserializeObject(json) as Dictionary<string, object>;
            lyric = GetString(obj, "lyric");
            if (string.IsNullOrWhiteSpace(lyric)) return string.Empty;

            try
            {
                return Encoding.UTF8.GetString(Convert.FromBase64String(lyric));
            }
            catch
            {
                return lyric;
            }
        }

        private static int Score(LyricsRecord record, Options options)
        {
            var score = 0;
            var wantedTitle = NormalizeForMatch(options.Title);
            var wantedArtist = NormalizeForMatch(options.Artist);
            var wantedAlbum = NormalizeForMatch(options.Album);
            var title = NormalizeForMatch(record.TrackName);
            var artist = NormalizeForMatch(record.ArtistName);
            var album = NormalizeForMatch(record.AlbumName);

            if (title == wantedTitle) score += 100;
            else if (ContainsEither(title, wantedTitle)) score += 40;

            if (!string.IsNullOrEmpty(wantedArtist))
            {
                if (artist == wantedArtist) score += 80;
                else if (ContainsEither(artist, wantedArtist)) score += 25;
            }

            if (!string.IsNullOrEmpty(wantedAlbum))
            {
                if (album == wantedAlbum) score += 30;
                else if (ContainsEither(album, wantedAlbum)) score += 10;
            }

            if (options.DurationSeconds > 0 && record.DurationSeconds > 0)
            {
                var diff = Math.Abs(record.DurationSeconds - options.DurationSeconds);
                if (diff <= 2) score += 50;
                else if (diff <= 5) score += 25;
                else if (diff <= 10) score += 5;
                else score -= Math.Min(40, diff);
            }

            if (!string.IsNullOrWhiteSpace(record.SyncedLyrics)) score += 20;
            if (record.Instrumental) score -= 100;
            return score;
        }

        private static string HttpGet(string url)
        {
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.Method = "GET";
            request.UserAgent = UserAgent;
            request.Accept = "application/json";
            request.Referer = "https://y.qq.com/";
            request.Timeout = 15000;
            request.ReadWriteTimeout = 15000;
            using (var response = (HttpWebResponse)request.GetResponse())
            using (var stream = response.GetResponseStream())
            using (var reader = new StreamReader(stream, Encoding.UTF8))
            {
                return reader.ReadToEnd();
            }
        }

        private static string HttpPostJson(string url, string json)
        {
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.Method = "POST";
            request.UserAgent = UserAgent;
            request.Accept = "application/json";
            request.Referer = "https://y.qq.com/";
            request.ContentType = "application/json; charset=utf-8";
            request.Timeout = 15000;
            request.ReadWriteTimeout = 15000;

            var bytes = Encoding.UTF8.GetBytes(json ?? string.Empty);
            request.ContentLength = bytes.Length;
            using (var stream = request.GetRequestStream())
            {
                stream.Write(bytes, 0, bytes.Length);
            }

            using (var response = (HttpWebResponse)request.GetResponse())
            using (var stream = response.GetResponseStream())
            using (var reader = new StreamReader(stream, Encoding.UTF8))
            {
                return reader.ReadToEnd();
            }
        }

        private static Options ParseArgs(string[] args)
        {
            var options = new Options();
            for (var i = 0; i < args.Length; i++)
            {
                var arg = args[i];
                if (arg == "--help" || arg == "-h" || arg == "/?") options.Help = true;
                else if (arg == "--title") options.Title = Next(args, ref i, arg);
                else if (arg == "--artist") options.Artist = Next(args, ref i, arg);
                else if (arg == "--album") options.Album = Next(args, ref i, arg);
                else if (arg == "--out") options.OutDir = Next(args, ref i, arg);
                else if (arg == "--sources") options.Sources = Next(args, ref i, arg);
                else if (arg == "--file-name") options.FileName = Next(args, ref i, arg);
                else if (arg == "--duration") options.DurationSeconds = ParseDuration(Next(args, ref i, arg));
                else if (arg == "--cached-only") options.CachedOnly = true;
                else if (arg == "--search-only") options.SearchOnly = true;
                else if (arg == "--list") options.ListOnly = true;
                else throw new ArgumentException("Unknown argument: " + arg);
            }
            return options;
        }

        private static bool IsSourceEnabled(Options options, string source)
        {
            var sources = options.Sources ?? string.Empty;
            foreach (var raw in sources.Split(','))
            {
                var part = (raw ?? string.Empty).Trim().ToLowerInvariant();
                if (part == source) return true;
            }
            return false;
        }

        private static string Next(string[] args, ref int index, string name)
        {
            if (index + 1 >= args.Length) throw new ArgumentException("Missing value for " + name);
            index++;
            return args[index];
        }

        private static int ParseDuration(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return 0;
            double seconds;
            if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out seconds))
            {
                return Math.Max(0, (int)Math.Round(seconds));
            }
            if (TimeSpan.TryParse(value, CultureInfo.InvariantCulture, out var ts))
            {
                return Math.Max(0, (int)Math.Round(ts.TotalSeconds));
            }
            throw new ArgumentException("Invalid --duration value: " + value);
        }

        private static string Url(string value)
        {
            return Uri.EscapeDataString(value ?? string.Empty).Replace("%20", "+");
        }

        private static string BuildFileName(string recordArtist, string recordTitle, string fallbackArtist, string fallbackTitle)
        {
            var artist = string.IsNullOrWhiteSpace(recordArtist) ? fallbackArtist : recordArtist;
            var title = string.IsNullOrWhiteSpace(recordTitle) ? fallbackTitle : recordTitle;
            if (string.IsNullOrWhiteSpace(artist)) return title + ".lrc";
            return artist + " - " + title + ".lrc";
        }

        private static string SanitizeFileName(string name)
        {
            foreach (var c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '_');
            return name.Trim();
        }

        private static string NormalizeNewlines(string text)
        {
            return (text ?? string.Empty).Replace("\r\n", "\n").Replace("\r", "\n").Replace("\n", Environment.NewLine);
        }

        private static string StripEnhancedLrcTags(string text)
        {
            text = Regex.Replace(text ?? string.Empty, @"<\d+:\d{2}(?:[.:]\d{1,3})?>", string.Empty);
            text = Regex.Replace(text, @"\(\d+,\d+\)", string.Empty);
            return text;
        }

        private static string FirstUsableLyric(params string[] values)
        {
            foreach (var value in values)
            {
                var lyric = ConvertPossibleQqLyricToLrc(value);
                if (IsUsableLrc(lyric)) return lyric;
            }
            return string.Empty;
        }

        private static string ConvertPossibleQqLyricToLrc(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return string.Empty;

            var rawText = value.Trim();
            if (QqQrcDecoder.LooksEncryptedQrc(rawText))
            {
                var decrypted = QqQrcDecoder.DecryptHexToText(rawText);
                var decryptedLrc = ConvertDecryptedQrcToLrc(decrypted);
                if (IsUsableLrc(decryptedLrc)) return decryptedLrc;
            }

            var text = DecodePossibleBase64(rawText);
            if (IsUsableLrc(text)) return text;

            var qrc = ConvertQrcTextToLrc(text);
            if (IsUsableLrc(qrc)) return qrc;

            if (QqQrcDecoder.LooksEncryptedQrc(text))
            {
                var decrypted = QqQrcDecoder.DecryptHexToText(text);
                var decryptedLrc = ConvertDecryptedQrcToLrc(decrypted);
                if (IsUsableLrc(decryptedLrc)) return decryptedLrc;
            }

            var hexText = DecodePossibleHexUtf8(text);
            if (!string.Equals(hexText, text, StringComparison.Ordinal))
            {
                if (IsUsableLrc(hexText)) return hexText;
                qrc = ConvertQrcTextToLrc(hexText);
                if (IsUsableLrc(qrc)) return qrc;
            }

            return string.Empty;
        }
        private static string ConvertDecryptedQrcToLrc(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return string.Empty;
            if (IsUsableLrc(text)) return text;

            var lyricContent = ExtractQrcLyricContent(text);
            if (!string.IsNullOrWhiteSpace(lyricContent))
            {
                if (IsUsableLrc(lyricContent)) return lyricContent;
                var converted = ConvertQrcTextToLrc(lyricContent);
                if (IsUsableLrc(converted)) return converted;
            }

            return ConvertQrcTextToLrc(text);
        }

        private static string ExtractQrcLyricContent(string text)
        {
            try
            {
                var doc = new XmlDocument { XmlResolver = null };
                doc.LoadXml(text.Trim());
                var nodes = doc.GetElementsByTagName("Lyric_1");
                foreach (XmlNode node in nodes)
                {
                    var attr = node.Attributes == null ? null : node.Attributes["LyricContent"];
                    if (attr != null && !string.IsNullOrWhiteSpace(attr.Value)) return attr.Value;
                }
            }
            catch
            {
            }

            var match = Regex.Match(text, "LyricContent=\"([^\"]*)\"", RegexOptions.Singleline);
            return match.Success ? WebUtility.HtmlDecode(match.Groups[1].Value) : string.Empty;
        }
        private static string ConvertQrcTextToLrc(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return string.Empty;
            var sb = new StringBuilder();
            var matches = Regex.Matches(text, @"\[(\d+),(\d+)\]([^\r\n]*)");
            foreach (Match match in matches)
            {
                var line = Regex.Replace(match.Groups[3].Value, @"\(\d+,\d+\)", string.Empty).Trim();
                if (line.Length == 0) continue;

                int ms;
                if (!int.TryParse(match.Groups[1].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out ms)) continue;
                sb.Append(FormatLrcTime(ms));
                sb.Append(line);
                sb.AppendLine();
            }
            return sb.ToString();
        }

        private static string FormatLrcTime(int milliseconds)
        {
            if (milliseconds < 0) milliseconds = 0;
            var totalSeconds = milliseconds / 1000;
            var minutes = totalSeconds / 60;
            var seconds = totalSeconds % 60;
            var centiseconds = (milliseconds % 1000) / 10;
            return "[" + minutes.ToString("00", CultureInfo.InvariantCulture) + ":" +
                   seconds.ToString("00", CultureInfo.InvariantCulture) + "." +
                   centiseconds.ToString("00", CultureInfo.InvariantCulture) + "]";
        }

        private static bool IsUsableLrc(string text)
        {
            return !string.IsNullOrWhiteSpace(text) && Regex.IsMatch(text, @"\[\d{1,3}:\d{2}(?:[.:]\d{1,3})?\]");
        }

        private static string DecodePossibleBase64(string value)
        {
            if (string.IsNullOrWhiteSpace(value) || value.IndexOf('[') >= 0) return value;
            if (!Regex.IsMatch(value, @"^[A-Za-z0-9+/=\r\n]+$")) return value;
            try
            {
                return Encoding.UTF8.GetString(Convert.FromBase64String(Regex.Replace(value, @"\s+", string.Empty)));
            }
            catch
            {
                return value;
            }
        }

        private static string DecodePossibleHexUtf8(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return value;
            value = Regex.Replace(value.Trim(), @"\s+", string.Empty);
            if ((value.Length % 2) != 0 || !Regex.IsMatch(value, @"\A[0-9a-fA-F]+\z")) return value;

            try
            {
                var bytes = new byte[value.Length / 2];
                for (var i = 0; i < bytes.Length; i++) bytes[i] = byte.Parse(value.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                return Encoding.UTF8.GetString(bytes);
            }
            catch
            {
                return value;
            }
        }

        private static string ProcKeywords(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return string.Empty;
            value = Regex.Replace(value, @"\(.*?\)|\[.*?\]|\{.*?\}", " ");
            var sb = new StringBuilder(value.Length);
            var lastSpace = false;
            foreach (var ch in value.ToLowerInvariant())
            {
                if (char.IsLetterOrDigit(ch))
                {
                    sb.Append(ch);
                    lastSpace = false;
                }
                else if (char.IsWhiteSpace(ch) && !lastSpace)
                {
                    sb.Append(' ');
                    lastSpace = true;
                }
            }
            return sb.ToString().Trim();
        }

        private static string NeteaseLinuxApiPost(string method, string targetUrl, Dictionary<string, object> parameters)
        {
            var wrapper = new Dictionary<string, object>
            {
                { "method", method },
                { "url", targetUrl },
                { "params", parameters ?? new Dictionary<string, object>() }
            };
            var json = new JavaScriptSerializer().Serialize(wrapper);
            var encrypted = AesEcbPkcs7Hex(json, "rFgB&h#%2?^eDg:Q");
            return HttpPostForm(
                "https://music.163.com/api/linux/forward",
                "eparams=" + Uri.EscapeDataString(encrypted),
                "https://music.163.com",
                "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/60.0.3112.90 Safari/537.36");
        }

        private static string AesEcbPkcs7Hex(string text, string key)
        {
            using (var aes = Aes.Create())
            {
                aes.Mode = CipherMode.ECB;
                aes.Padding = PaddingMode.PKCS7;
                aes.Key = Encoding.UTF8.GetBytes(key);
                using (var encryptor = aes.CreateEncryptor())
                {
                    var bytes = Encoding.UTF8.GetBytes(text ?? string.Empty);
                    var encrypted = encryptor.TransformFinalBlock(bytes, 0, bytes.Length);
                    var sb = new StringBuilder(encrypted.Length * 2);
                    foreach (var b in encrypted) sb.Append(b.ToString("X2", CultureInfo.InvariantCulture));
                    return sb.ToString();
                }
            }
        }

        private static string HttpPostForm(string url, string formBody, string referer, string userAgent, Encoding responseEncoding = null)
        {
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.Method = "POST";
            request.UserAgent = string.IsNullOrWhiteSpace(userAgent) ? UserAgent : userAgent;
            request.Accept = "application/json";
            request.Referer = referer;
            request.ContentType = "application/x-www-form-urlencoded";
            request.Timeout = 15000;
            request.ReadWriteTimeout = 15000;

            var bytes = Encoding.UTF8.GetBytes(formBody ?? string.Empty);
            request.ContentLength = bytes.Length;
            using (var stream = request.GetRequestStream())
            {
                stream.Write(bytes, 0, bytes.Length);
            }

            using (var response = (HttpWebResponse)request.GetResponse())
            using (var stream = response.GetResponseStream())
            using (var reader = new StreamReader(stream, responseEncoding ?? Encoding.UTF8))
            {
                return reader.ReadToEnd();
            }
        }

        private static string CleanNeteaseLyric(string lyric)
        {
            if (string.IsNullOrWhiteSpace(lyric)) return string.Empty;
            lyric = DecodeNeteaseText(lyric);
            return Regex.Replace(lyric, @"^\{(.*)\}\s*", string.Empty, RegexOptions.Multiline).Trim();
        }

        private static string DecodeNeteaseText(string value)
        {
            if (string.IsNullOrEmpty(value)) return string.Empty;
            try
            {
                var fixedText = Encoding.UTF8.GetString(Encoding.GetEncoding("GB18030").GetBytes(value));
                return LooksLikeMojibake(value) && !LooksLikeMojibake(fixedText) ? fixedText : value;
            }
            catch
            {
                return value;
            }
        }

        private static bool LooksLikeMojibake(string value)
        {
            if (string.IsNullOrEmpty(value)) return false;
            var count = 0;
            foreach (var ch in value)
            {
                if (ch == '?' || ch == '?' || ch == '?' || ch == '?' || ch == '?' || ch == '?' || ch == '?' || ch == '?' || ch == '?') count++;
            }
            return count > 0 || Regex.IsMatch(value, @"[閸?榭縘{2,}");
        }

        private static string BuildNeteaseArtistText(object[] artists)
        {
            if (artists == null || artists.Length == 0) return string.Empty;
            var names = new List<string>();
            foreach (var entry in artists)
            {
                var item = entry as Dictionary<string, object>;
                var name = DecodeNeteaseText(GetString(item, "name"));
                if (!string.IsNullOrWhiteSpace(name)) names.Add(name);
            }
            return string.Join("/", names.ToArray());
        }

        private static int NeteaseDurationToSeconds(int duration)
        {
            if (duration <= 0) return 0;
            return duration > 10000 ? (int)Math.Round(duration / 1000.0) : duration;
        }



        private static string Base64Utf8(string value)
        {
            return Convert.ToBase64String(Encoding.UTF8.GetBytes(value ?? string.Empty));
        }

        private static long CurrentUnixMilliseconds()
        {
            return (long)(DateTime.UtcNow - new DateTime(1970, 1, 1)).TotalMilliseconds;
        }

        private static string NormalizeForMatch(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return string.Empty;
            value = value.ToLowerInvariant();
            var sb = new StringBuilder(value.Length);
            foreach (var ch in value)
            {
                if (char.IsLetterOrDigit(ch)) sb.Append(ch);
            }
            return sb.ToString();
        }



        private static string GetFirstCData(XmlDocument doc, string tagName)
        {
            if (doc == null) return string.Empty;
            var nodes = doc.GetElementsByTagName(tagName);
            if (nodes == null || nodes.Count == 0) return string.Empty;
            return nodes[0].InnerText ?? string.Empty;
        }

        private static string GetChildText(XmlNode node, string tagName)
        {
            if (node == null) return string.Empty;
            foreach (XmlNode child in node.ChildNodes)
            {
                if (string.Equals(child.Name, tagName, StringComparison.OrdinalIgnoreCase)) return child.InnerText ?? string.Empty;
            }
            return string.Empty;
        }

        private static string DecodePercent(string value)
        {
            if (string.IsNullOrEmpty(value)) return string.Empty;
            try { return Uri.UnescapeDataString(value.Replace("+", "%2B")); }
            catch { return value; }
        }


        private static string GetString(Dictionary<string, object> dict, string name)
        {
            object value;
            return dict != null && dict.TryGetValue(name, out value) && value != null ? Convert.ToString(value, CultureInfo.InvariantCulture) : string.Empty;
        }

        private static int GetInt(Dictionary<string, object> dict, string name)
        {
            object value;
            if (dict == null || !dict.TryGetValue(name, out value) || value == null) return 0;
            try { return Convert.ToInt32(value, CultureInfo.InvariantCulture); }
            catch { return 0; }
        }

        private static Dictionary<string, object> GetDict(Dictionary<string, object> dict, string name)
        {
            object value;
            return dict != null && dict.TryGetValue(name, out value) ? value as Dictionary<string, object> : null;
        }

        private static object[] GetArray(Dictionary<string, object> dict, string name)
        {
            object value;
            return dict != null && dict.TryGetValue(name, out value) ? value as object[] : null;
        }

        private static string BuildSingerText(object[] singers)
        {
            if (singers == null || singers.Length == 0) return string.Empty;
            var names = new List<string>();
            foreach (var entry in singers)
            {
                var item = entry as Dictionary<string, object>;
                var name = DecodeNeteaseText(GetString(item, "name"));
                if (!string.IsNullOrWhiteSpace(name)) names.Add(name);
            }
            return string.Join("/", names.ToArray());
        }

        private static string FirstNonEmpty(params string[] values)
        {
            foreach (var value in values)
            {
                if (!string.IsNullOrWhiteSpace(value)) return value;
            }
            return string.Empty;
        }

        private static bool ContainsEither(string a, string b)
        {
            if (string.IsNullOrEmpty(a) || string.IsNullOrEmpty(b)) return false;
            return a.Contains(b) || b.Contains(a);
        }

        private static void PrintUsage()
        {
            Console.WriteLine("LrcDownloader 0.1 (.NET Framework 4.8)");
            Console.WriteLine("Usage:");
            Console.WriteLine("  LrcDownloader.exe --title <title> --artist <artist> --album <album> --duration <seconds> --out <folder>");
            Console.WriteLine("Options:");
            Console.WriteLine("  --title       Track title. Required.");
            Console.WriteLine("  --artist      Artist name.");
            Console.WriteLine("  --album       Album name.");
            Console.WriteLine("  --duration    Track duration in seconds, for example 233 or 233.4.");
            Console.WriteLine("  --out         Output folder. Required.");
            Console.WriteLine("  --file-name   Optional output LRC file name.");
            Console.WriteLine("  --sources     Comma separated lyric sources: lrclib,qq1,qq2,netease. Empty means no online download. qq and 163 are compatibility aliases.");
            Console.WriteLine("  --cached-only Do not call LRCLIB external lookup endpoint.");
            Console.WriteLine("  --search-only Skip exact signature lookup and only use search.");
            Console.WriteLine("  --list        Search and print tab-separated results without downloading.");
            Console.WriteLine("Exit codes: 0 success, 1 not found, 2 bad arguments, 3 error.");
        }

        private sealed class Options
        {
            public string Title;
            public string Artist;
            public string Album;
            public int DurationSeconds;
            public string OutDir;
            public string FileName;
            public string Sources = "lrclib,qq1";
            public bool CachedOnly;
            public bool SearchOnly;
            public bool ListOnly;
            public bool Help;
        }

        private sealed class LyricsRecord
        {
            public string TrackName;
            public string ArtistName;
            public string AlbumName;
            public int DurationSeconds;
            public bool Instrumental;
            public string SyncedLyrics;
            public string SourceId;
            public int SourceNumericId;
            public string SourceKey;
            public string SourceDisplayName;
            public int ScoreValue;

            public static LyricsRecord FromDictionary(IDictionary<string, object> dict)
            {
                if (dict == null) return null;
                return new LyricsRecord
                {
                    TrackName = GetString(dict, "trackName"),
                    ArtistName = GetString(dict, "artistName"),
                    AlbumName = GetString(dict, "albumName"),
                    DurationSeconds = GetInt(dict, "duration"),
                    Instrumental = GetBool(dict, "instrumental"),
                    SyncedLyrics = GetString(dict, "syncedLyrics")
                };
            }

            private static string GetString(IDictionary<string, object> dict, string name)
            {
                object value;
                return dict.TryGetValue(name, out value) && value != null ? Convert.ToString(value, CultureInfo.InvariantCulture) : string.Empty;
            }

            private static int GetInt(IDictionary<string, object> dict, string name)
            {
                object value;
                if (!dict.TryGetValue(name, out value) || value == null) return 0;
                try { return Convert.ToInt32(value, CultureInfo.InvariantCulture); }
                catch { return 0; }
            }

            private static bool GetBool(IDictionary<string, object> dict, string name)
            {
                object value;
                if (!dict.TryGetValue(name, out value) || value == null) return false;
                try { return Convert.ToBoolean(value, CultureInfo.InvariantCulture); }
                catch { return false; }
            }
        }
    }
}









