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

                if (options.SelfTest) return RunSelfTests();

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

                var queryOptions = BuildQueryOptions(options);
                var record = options.CandidateIndex >= 0
                    ? FindLyricsCandidate(queryOptions[0], options.CandidateIndex)
                    : FindBestLyrics(queryOptions[0]);
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
                AppendManifest(options.ManifestPath, outputPath);

                Console.WriteLine(outputPath);
                Console.WriteLine("SELECTED:\t" + SafeTsv(record.TrackName) + "\t" + SafeTsv(record.ArtistName));
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
            var query = BuildQueryOptions(options)[0];
            var results = SearchCandidateSources(query);
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

        private static List<LyricsRecord> SearchCandidateSources(Options options)
        {
            var cached = TryLoadCandidateCache(options);
            if (cached != null) return cached;

            var results = SearchAllSources(options);
            SaveCandidateCache(options, results);
            return results;
        }

        private static List<LyricsRecord> TryLoadCandidateCache(Options options)
        {
            if (string.IsNullOrWhiteSpace(options.CandidateCachePath) || !File.Exists(options.CandidateCachePath)) return null;
            try
            {
                var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
                var cache = serializer.Deserialize<CandidateCache>(File.ReadAllText(options.CandidateCachePath, Encoding.UTF8));
                if (cache == null || cache.Records == null) return null;
                if (DateTime.UtcNow.Ticks - cache.CreatedUtcTicks > TimeSpan.FromMinutes(30).Ticks) return null;
                if (!string.Equals(cache.Title, NormalizeForMatch(options.Title), StringComparison.Ordinal) ||
                    !string.Equals(cache.Artist, options.TitleOnly ? string.Empty : NormalizeForMatch(options.Artist), StringComparison.Ordinal) ||
                    !string.Equals(cache.Album, NormalizeForMatch(options.Album), StringComparison.Ordinal) ||
                    !string.Equals(cache.Sources, NormalizeSources(options.Sources), StringComparison.Ordinal) ||
                    cache.DurationSeconds != options.DurationSeconds || cache.TitleOnly != options.TitleOnly) return null;
                return cache.Records;
            }
            catch
            {
                return null;
            }
        }

        private static void SaveCandidateCache(Options options, List<LyricsRecord> records)
        {
            if (string.IsNullOrWhiteSpace(options.CandidateCachePath) || records == null) return;
            try
            {
                var directory = Path.GetDirectoryName(options.CandidateCachePath);
                if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
                var cache = new CandidateCache
                {
                    Title = NormalizeForMatch(options.Title),
                    Artist = options.TitleOnly ? string.Empty : NormalizeForMatch(options.Artist),
                    Album = NormalizeForMatch(options.Album),
                    Sources = NormalizeSources(options.Sources),
                    DurationSeconds = options.DurationSeconds,
                    TitleOnly = options.TitleOnly,
                    CreatedUtcTicks = DateTime.UtcNow.Ticks,
                    Records = records
                };
                var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
                var temporaryPath = options.CandidateCachePath + "." + Guid.NewGuid().ToString("N") + ".tmp";
                File.WriteAllText(temporaryPath, serializer.Serialize(cache), new UTF8Encoding(false));
                try
                {
                    if (File.Exists(options.CandidateCachePath)) File.Delete(options.CandidateCachePath);
                    File.Move(temporaryPath, options.CandidateCachePath);
                }
                finally
                {
                    if (File.Exists(temporaryPath)) File.Delete(temporaryPath);
                }
            }
            catch
            {
            }
        }

        private static string NormalizeSources(string sources)
        {
            var parts = new List<string>();
            foreach (var raw in (sources ?? string.Empty).Split(','))
            {
                var part = (raw ?? string.Empty).Trim().ToLowerInvariant();
                if (part.Length > 0 && !parts.Contains(part)) parts.Add(part);
            }
            parts.Sort(StringComparer.Ordinal);
            return string.Join(",", parts.ToArray());
        }

        private static void TryAppendSearchResults(List<LyricsRecord> target, Func<List<LyricsRecord>> search, string sourceKey, string sourceName, Options options)
        {
            try
            {
                foreach (var item in search())
                {
                    if (item == null || string.IsNullOrWhiteSpace(item.TrackName)) continue;
                    if (!IsAcceptableCandidate(item, options)) continue;
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
                    if (exact != null && !string.IsNullOrWhiteSpace(exact.SyncedLyrics) &&
                        IsAcceptableCandidate(exact, options) && LyricsHeaderMatchesRequest(exact.SyncedLyrics, options)) return exact;

                    if (!options.CachedOnly)
                    {
                        exact = TryGetBySignature(options, cached: false);
                        if (exact != null && !string.IsNullOrWhiteSpace(exact.SyncedLyrics) &&
                            IsAcceptableCandidate(exact, options) && LyricsHeaderMatchesRequest(exact.SyncedLyrics, options)) return exact;
                    }
                }

                var results = Search(options);
                LyricsRecord best = null;
                var bestScore = int.MinValue;
                foreach (var item in results)
                {
                    if (string.IsNullOrWhiteSpace(item.SyncedLyrics)) continue;
                    if (!IsAcceptableCandidate(item, options)) continue;
                    if (!LyricsHeaderMatchesRequest(item.SyncedLyrics, options)) continue;
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

        private static List<Options> BuildQueryOptions(Options original)
        {
            var rawTitle = (original.Title ?? string.Empty).Trim();
            var rawArtist = (original.Artist ?? string.Empty).Trim();
            var preferredTitle = CleanTitleText(rawTitle);
            var preferredArtist = IsSourceLikeArtist(rawArtist) ? string.Empty : CleanArtistText(rawArtist);

            string extractedTitle;
            string extractedArtist;
            if (TryExtractExplicitArtist(rawTitle, out extractedTitle, out extractedArtist))
            {
                preferredTitle = extractedTitle;
                preferredArtist = extractedArtist;
            }
            else if (TrySplitCombinedArtistTitle(rawTitle, rawArtist, out extractedTitle, out extractedArtist))
            {
                preferredTitle = extractedTitle;
                preferredArtist = extractedArtist;
            }

            if (string.IsNullOrWhiteSpace(preferredTitle)) preferredTitle = rawTitle;
            var hasArtist = !string.IsNullOrWhiteSpace(preferredArtist);
            return new List<Options>
            {
                CloneOptions(original, preferredTitle, hasArtist ? preferredArtist : string.Empty, original.TitleOnly || !hasArtist)
            };
        }

        private static Options CloneOptions(Options source, string title, string artist, bool titleOnly)
        {
            return new Options
            {
                Title = title,
                Artist = artist,
                Album = source.Album,
                DurationSeconds = source.DurationSeconds,
                OutDir = source.OutDir,
                FileName = source.FileName,
                ManifestPath = source.ManifestPath,
                CandidateCachePath = source.CandidateCachePath,
                Sources = source.Sources,
                CachedOnly = source.CachedOnly,
                SearchOnly = source.SearchOnly,
                ListOnly = source.ListOnly,
                SelfTest = source.SelfTest,
                TitleOnly = titleOnly,
                CandidateIndex = source.CandidateIndex,
                Help = source.Help
            };
        }

        private static bool TryExtractExplicitArtist(string rawTitle, out string title, out string artist)
        {
            title = string.Empty;
            artist = string.Empty;
            if (string.IsNullOrWhiteSpace(rawTitle)) return false;

            var match = Regex.Match(rawTitle,
                @"(?:^|[\s\u3000|_/])(?:艺术家|歌手|演唱|artist)\s*[:：]?\s*(?<artist>.+?)\s*$",
                RegexOptions.IgnoreCase);
            if (!match.Success || match.Index <= 0) return false;

            title = CleanTitleText(rawTitle.Substring(0, match.Index));
            artist = CleanArtistText(match.Groups["artist"].Value);
            return title.Length > 0 && artist.Length > 0;
        }

        private static bool TrySplitCombinedArtistTitle(string rawTitle, string rawArtist, out string title, out string artist)
        {
            title = string.Empty;
            artist = string.Empty;
            if (string.IsNullOrWhiteSpace(rawTitle)) return false;

            Match match = null;
            if (string.IsNullOrWhiteSpace(rawArtist))
                match = Regex.Match(rawTitle, @"^\s*(?<artist>.+?)\s+[-–—－]\s+(?<title>.+?)\s*$");
            else if (IsSourceLikeArtist(rawArtist))
                match = Regex.Match(rawTitle, @"^\s*(?<artist>[^-–—－]{1,80})[-–—－](?<title>.+?)\s*$");

            if (match == null || !match.Success) return false;
            title = CleanTitleText(match.Groups["title"].Value);
            artist = CleanArtistText(match.Groups["artist"].Value);
            return title.Length > 0 && artist.Length > 0;
        }

        private static string CleanTitleText(string value)
        {
            value = (value ?? string.Empty).Trim();
            value = Regex.Replace(value, @"\.(?:mp3|flac|wav|ape|m4a|aac|ogg|wma)$", string.Empty, RegexOptions.IgnoreCase);
            value = Regex.Replace(value, @"^\s*\d{1,3}\s*[._-]+\s*", string.Empty);
            value = Regex.Replace(value,
                @"[\s·・\-–—]*《[^》]+》\s*(?:(?:电影|电视剧|网络剧|网剧|动画|动漫|游戏)\s*)?(?:主题曲|片尾曲|片头曲|插曲|推广曲|宣传曲|原声带?)?\s*$",
                string.Empty, RegexOptions.IgnoreCase);
            value = Regex.Replace(value,
                @"\s*(?:(?:电影|电视剧|网络剧|网剧|动画|动漫|游戏)\s*)?(?:主题曲|片尾曲|片头曲|插曲|推广曲|宣传曲)\s*$",
                string.Empty, RegexOptions.IgnoreCase);
            value = Regex.Replace(value,
                @"[\s_-]*(?:[\(（\[【]\s*)?(?:instrumental|伴奏|karaoke|off\s*vocal|无和声|纯音乐)(?:\s*[\)）\]】])?\s*$",
                string.Empty, RegexOptions.IgnoreCase);
            return value.Trim(' ', '\t', '\u3000', '_', '-', '\u2013', '\u2014', '\u00B7', '\u30FB');
        }

        private static string CleanArtistText(string value)
        {
            value = (value ?? string.Empty).Trim();
            value = Regex.Replace(value, @"\.(?:mp3|flac|wav|ape|m4a|aac|ogg|wma)$", string.Empty, RegexOptions.IgnoreCase);
            value = Regex.Replace(value,
                @"[\s_-]*(?:[\(（\[【]\s*)?(?:instrumental|伴奏|karaoke|off\s*vocal|无和声|纯音乐)(?:\s*[\)）\]】])?\s*$",
                string.Empty, RegexOptions.IgnoreCase);
            return value.Trim(' ', '\t', '\u3000', '_', '-', '\u2013', '\u2014');
        }

        private static bool IsSourceLikeArtist(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return false;
            return Regex.IsMatch(value,
                @"伴奏网|伴奏网站|伴奏下载|立体声伴奏|音乐下载|歌曲下载|音乐网|资源网|铃声网|mp3|www\.|https?://|\.(?:com|net|cn)\b",
                RegexOptions.IgnoreCase);
        }

        private static LyricsRecord FindLyricsCandidate(Options options, int requestedIndex)
        {
            var validIndex = 0;
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var candidate in SearchCandidateSources(options))
            {
                var identity = NormalizeForMatch(candidate.TrackName) + "|" + NormalizeForMatch(candidate.ArtistName);
                if (!seen.Add(identity)) continue;
                try
                {
                    var resolved = ResolveSearchCandidate(candidate, options);
                    if (resolved == null || string.IsNullOrWhiteSpace(resolved.SyncedLyrics)) continue;
                    if (!LyricsHeaderMatchesRequest(resolved.SyncedLyrics, options)) continue;
                    if (validIndex++ == requestedIndex) return resolved;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("WARN: candidate download failed: " + ex.Message);
                }
            }
            return null;
        }

        private static LyricsRecord ResolveSearchCandidate(LyricsRecord candidate, Options options)
        {
            if (candidate == null) return null;
            if (string.Equals(candidate.SourceKey, "lrclib", StringComparison.OrdinalIgnoreCase)) return candidate;
            if (string.Equals(candidate.SourceKey, "qq1", StringComparison.OrdinalIgnoreCase))
            {
                if (!string.IsNullOrWhiteSpace(candidate.SourceId)) candidate.SyncedLyrics = GetQqMusicLyric(candidate.SourceId);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics)) candidate.SyncedLyrics = GetQqMusicLyricDownload(candidate);
                return candidate;
            }
            if (string.Equals(candidate.SourceKey, "qq2", StringComparison.OrdinalIgnoreCase))
            {
                candidate.SyncedLyrics = GetQqMusicLyricFromPlayLyricInfo(candidate, options);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics)) candidate.SyncedLyrics = GetQqMusicLyricDownload(candidate);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics) && !string.IsNullOrWhiteSpace(candidate.SourceId))
                    candidate.SyncedLyrics = GetQqMusicLyric(candidate.SourceId);
                return candidate;
            }
            if (string.Equals(candidate.SourceKey, "netease", StringComparison.OrdinalIgnoreCase))
            {
                if (!string.IsNullOrWhiteSpace(candidate.SourceId)) candidate.SyncedLyrics = GetNeteaseMusicLyric(candidate.SourceId);
                return candidate;
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
            candidates.Sort((left, right) => Score(right, options).CompareTo(Score(left, options)));
            foreach (var candidate in candidates)
            {
                if (!IsAcceptableCandidate(candidate, options)) continue;
                if (!string.IsNullOrWhiteSpace(candidate.SourceId)) candidate.SyncedLyrics = GetQqMusicLyric(candidate.SourceId);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics)) candidate.SyncedLyrics = GetQqMusicLyricDownload(candidate);
                if (!string.IsNullOrWhiteSpace(candidate.SyncedLyrics) && LyricsHeaderMatchesRequest(candidate.SyncedLyrics, options)) return candidate;
            }
            return null;
        }

        private static LyricsRecord TryGetFromQqMusicQrcSource(Options options)
        {
            var candidates = SearchQqMusicPcLrc(options);
            if (candidates.Count == 0) candidates = SearchQqMusic(options);
            candidates.Sort((left, right) => Score(right, options).CompareTo(Score(left, options)));

            foreach (var candidate in candidates)
            {
                if (!IsAcceptableCandidate(candidate, options)) continue;
                candidate.SyncedLyrics = GetQqMusicLyricFromPlayLyricInfo(candidate, options);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics)) candidate.SyncedLyrics = GetQqMusicLyricDownload(candidate);
                if (string.IsNullOrWhiteSpace(candidate.SyncedLyrics) && !string.IsNullOrWhiteSpace(candidate.SourceId))
                {
                    candidate.SyncedLyrics = GetQqMusicLyric(candidate.SourceId);
                }
                if (!string.IsNullOrWhiteSpace(candidate.SyncedLyrics) && LyricsHeaderMatchesRequest(candidate.SyncedLyrics, options)) return candidate;
            }

            return null;
        }

        private static LyricsRecord TryGetFromNeteaseMusic(Options options)
        {
            var candidates = SearchNeteaseMusic(options);
            candidates.Sort((left, right) => Score(right, options).CompareTo(Score(left, options)));

            foreach (var candidate in candidates)
            {
                if (!IsAcceptableCandidate(candidate, options)) continue;
                if (string.IsNullOrWhiteSpace(candidate.SourceId)) continue;
                candidate.SyncedLyrics = GetNeteaseMusicLyric(candidate.SourceId);
                if (!string.IsNullOrWhiteSpace(candidate.SyncedLyrics) && LyricsHeaderMatchesRequest(candidate.SyncedLyrics, options)) return candidate;
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
                foreach (XmlNode song in songInfo)
                {
                    var idText = song.Attributes != null && song.Attributes["id"] != null ? song.Attributes["id"].Value : string.Empty;
                    int songId;
                    if (!int.TryParse(idText, NumberStyles.Integer, CultureInfo.InvariantCulture, out songId) || songId <= 0) continue;
                    var title = DecodePercent(GetChildText(song, "name"));
                    var artist = DecodePercent(GetChildText(song, "singername"));
                    var album = DecodePercent(GetChildText(song, "albumname"));
                    if (string.IsNullOrWhiteSpace(title)) continue;
                    if (!string.IsNullOrWhiteSpace(options.Artist) && string.IsNullOrWhiteSpace(artist)) continue;

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

        private static bool TitleMatches(string candidateTitle, string wantedTitle)
        {
            var candidate = NormalizeForMatch(candidateTitle);
            var wanted = NormalizeForMatch(wantedTitle);
            if (candidate.Length == 0 || wanted.Length == 0) return false;
            if (candidate == wanted) return true;
            return candidate.Length >= 2 && wanted.Length >= 2 && ContainsEither(candidate, wanted);
        }

        private static bool ArtistMatches(string candidateArtist, string wantedArtist)
        {
            var candidate = NormalizeForMatch(candidateArtist);
            var wanted = NormalizeForMatch(wantedArtist);
            if (candidate.Length == 0 || wanted.Length == 0) return false;
            if (ContainsEither(candidate, wanted)) return true;

            var parts = Regex.Split(wantedArtist ?? string.Empty, @"[/\uFF0F\u3001,&\uFF06\uFF0C;\uFF1B+]+|\b(?:feat|ft)\.?\b", RegexOptions.IgnoreCase);
            foreach (var part in parts)
            {
                var normalized = NormalizeForMatch(part);
                if (normalized.Length >= 2 && candidate.Contains(normalized)) return true;
            }
            return false;
        }

        private static bool IsAcceptableCandidate(LyricsRecord record, Options options)
        {
            if (record == null || !TitleMatches(record.TrackName, options.Title)) return false;
            if (options.TitleOnly) return true;
            if (string.IsNullOrWhiteSpace(options.Artist) || string.IsNullOrWhiteSpace(record.ArtistName)) return true;
            if (ArtistMatches(record.ArtistName, options.Artist)) return true;

            if (NormalizeForMatch(record.TrackName) == NormalizeForMatch(options.Title) &&
                options.DurationSeconds > 0 && record.DurationSeconds > 0)
            {
                return Math.Abs(record.DurationSeconds - options.DurationSeconds) <= 5;
            }
            return false;
        }

        private static bool LyricsHeaderMatchesRequest(string lyrics, Options options)
        {
            if (string.IsNullOrWhiteSpace(lyrics)) return false;

            var timedLineCount = Regex.Matches(lyrics, @"(?im)^\s*\[\d{1,3}:\d{2}(?:[\.:]\d{1,3})?\]").Count;
            if (timedLineCount <= 2 && Regex.IsMatch(lyrics, @"\u7eaf\u97f3\u4e50|\u6ca1\u6709\u586b\u8bcd|\binstrumental\b", RegexOptions.IgnoreCase)) return false;

            var titleTag = Regex.Match(lyrics, @"(?im)^\s*\[ti\s*:\s*([^\]]+)\]");
            if (titleTag.Success && !TitleMatches(titleTag.Groups[1].Value, options.Title)) return false;

            var timedLines = Regex.Matches(lyrics, @"(?im)^\s*\[(\d{1,3}):(\d{2})(?:[\.:]\d{1,3})?\]\s*(.*?)\s*$");
            var inspectedLines = 0;
            for (var lineIndex = 0; lineIndex < timedLines.Count; lineIndex++)
            {
                var match = timedLines[lineIndex];
                if (++inspectedLines > 10) break;
                int minutes;
                int seconds;
                if (!int.TryParse(match.Groups[1].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out minutes) ||
                    !int.TryParse(match.Groups[2].Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out seconds)) continue;
                if (minutes * 60 + seconds > 30) break;

                var text = match.Groups[3].Value.Trim();
                var separator = Regex.Match(text, @"\s[-\u2013\u2014]\s");
                if (!separator.Success || text.Length > 180) continue;

                var left = text.Substring(0, separator.Index).Trim();
                var right = text.Substring(separator.Index + separator.Length).Trim();
                if (TitleMatches(left, options.Title) || TitleMatches(right, options.Title)) return true;

                // A spaced dash can also be ordinary lyric punctuation. Treat it
                // as a mismatching title header only when nearby credit fields
                // confirm that this is actually the metadata block.
                var lastLookAhead = Math.Min(timedLines.Count - 1, lineIndex + 3);
                for (var nextIndex = lineIndex + 1; nextIndex <= lastLookAhead; nextIndex++)
                {
                    var nextText = timedLines[nextIndex].Groups[3].Value.Trim();
                    if (Regex.IsMatch(nextText,
                        @"^(?:作词|作詞|词|詞|作曲|曲|编曲|編曲|演唱|歌手|artist|lyrics?|music|composer|vocal)\s*[:\uFF1A\u2236]",
                        RegexOptions.IgnoreCase)) return false;
                }
            }
            return true;
        }

        private static int RunSelfTests()
        {
            var failed = 0;
            Action<bool, string> check = (condition, name) =>
            {
                if (condition) Console.WriteLine("PASS: " + name);
                else
                {
                    Console.Error.WriteLine("FAIL: " + name);
                    failed++;
                }
            };

            var options = new Options { Title = "手掌", Artist = "盛宇D-SHINE/加木" };
            check(LyricsHeaderMatchesRequest(
                "[00:00.00]手掌 (Live) - 盛宇D-SHINE/加木\n[00:01.00]作词：测试\n[00:05.00]这是正文", options),
                "matching title header is accepted");
            check(!LyricsHeaderMatchesRequest(
                "[00:00.00]小さな手のひら\n[00:01.00]いつかは誰かを包み込む\n[00:02.00]大きな手のひら\n[00:03.00]手のひら - 渡り廊下走り隊\n[00:04.00]詞∶カシアス島田\n[00:05.00]曲∶Voice of Mind", options),
                "mismatching delayed title header is rejected");
            check(!LyricsHeaderMatchesRequest("[00:00.00]纯音乐，请欣赏", options),
                "instrumental placeholder is rejected");
            check(LyricsHeaderMatchesRequest(
                "[00:00.00]I walk - alone tonight\n[00:05.00]and wait for morning light\n[00:10.00]this is an ordinary lyric", options),
                "ordinary lyric dash is not treated as a title header");

            var correct = new LyricsRecord { TrackName = "手掌 (Live)", ArtistName = "盛宇D-SHINE/加木" };
            var wrongArtist = new LyricsRecord { TrackName = "手掌", ArtistName = "渡り廊下走り隊" };
            check(IsAcceptableCandidate(correct, options), "matching title and artist candidate is accepted");
            check(!IsAcceptableCandidate(wrongArtist, options), "wrong artist candidate is rejected");
            check(IsAcceptableCandidate(wrongArtist, new Options { Title = "手掌", TitleOnly = true }),
                "title-only candidate switching accepts another artist");

            var explicitMetadataQueries = BuildQueryOptions(new Options
            {
                Title = "问花·《白蛇2：青蛇劫起》电影主题曲　艺术家周深_instrumental",
                Artist = string.Empty
            });
            check(explicitMetadataQueries.Count > 0 && explicitMetadataQueries[0].Title == "问花" &&
                  explicitMetadataQueries[0].Artist == "周深" && !explicitMetadataQueries[0].TitleOnly,
                "embedded artist metadata is extracted before title-only fallback");

            var sourceMetadataQueries = BuildQueryOptions(new Options
            {
                Title = "莫艳琳-带走",
                Artist = "超级星立体声伴奏网"
            });
            check(sourceMetadataQueries.Count > 0 && sourceMetadataQueries[0].Title == "带走" &&
                  sourceMetadataQueries[0].Artist == "莫艳琳" && !sourceMetadataQueries[0].TitleOnly,
                "source-site artist metadata is replaced by combined title pair");

            var normalMetadataQueries = BuildQueryOptions(new Options { Title = "情歌", Artist = "梁静茹" });
            check(normalMetadataQueries.Count > 0 && normalMetadataQueries[0].Title == "情歌" &&
                  normalMetadataQueries[0].Artist == "梁静茹" && !normalMetadataQueries[0].TitleOnly,
                "normal title and artist pair remains the first query");
            var switchQueries = BuildQueryOptions(new Options { Title = "情歌", Artist = "梁静茹", TitleOnly = true });
            check(switchQueries.Count > 0 && switchQueries[0].TitleOnly,
                "same-title candidate switching preserves title-only matching");
            check(!LooksLikeMojibake("你好吗？今天很好。"), "normal Chinese punctuation is not mojibake");
            check(LooksLikeMojibake("姝岃瘝鏃堕棿"), "known GBK mojibake sequence is detected");

            if (failed == 0)
            {
                Console.WriteLine("SELF_TEST_OK");
                return 0;
            }
            Console.Error.WriteLine("SELF_TEST_FAILED: " + failed.ToString(CultureInfo.InvariantCulture));
            return 3;
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
                else if (arg == "--manifest") options.ManifestPath = Next(args, ref i, arg);
                else if (arg == "--duration") options.DurationSeconds = ParseDuration(Next(args, ref i, arg));
                else if (arg == "--candidate-index") options.CandidateIndex = ParseCandidateIndex(Next(args, ref i, arg));
                else if (arg == "--candidate-cache") options.CandidateCachePath = Next(args, ref i, arg);
                else if (arg == "--cached-only") options.CachedOnly = true;
                else if (arg == "--search-only") options.SearchOnly = true;
                else if (arg == "--title-only") options.TitleOnly = true;
                else if (arg == "--list") options.ListOnly = true;
                else if (arg == "--self-test") options.SelfTest = true;
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

        private static int ParseCandidateIndex(string value)
        {
            int index;
            if (!int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out index) || index < 0)
                throw new ArgumentException("Invalid --candidate-index value: " + value);
            return index;
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

        private static void AppendManifest(string manifestPath, string outputPath)
        {
            if (string.IsNullOrWhiteSpace(manifestPath) || string.IsNullOrWhiteSpace(outputPath)) return;

            try
            {
                var directory = Path.GetDirectoryName(manifestPath);
                if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
                File.AppendAllText(manifestPath, Path.GetFullPath(outputPath) + "\r\n", new UTF8Encoding(false));
            }
            catch
            {
            }
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
            if (value.IndexOf('\uFFFD') >= 0 || value.Contains("锟斤拷")) return true;

            // These combinations are common when UTF-8 bytes were decoded as
            // GBK/GB18030. Requiring a sequence avoids treating normal Chinese
            // question marks or an isolated uncommon character as corruption.
            return Regex.IsMatch(value,
                @"(?:鈥[斺濈潃]|銆[併傘]|锛[屼岋紒]|閸[欙紝]|[閸榭縘]{2,}|鐨勪|浣犵殑|姝岃瘝|鏃堕棿)");
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
            Console.WriteLine("  --manifest    Optional manifest file to record downloaded temporary LRC paths.");
            Console.WriteLine("  --sources     Comma separated lyric sources: lrclib,qq1,qq2,netease. Empty means no online download. qq and 163 are compatibility aliases.");
            Console.WriteLine("  --cached-only Do not call LRCLIB external lookup endpoint.");
            Console.WriteLine("  --search-only Skip exact signature lookup and only use search.");
            Console.WriteLine("  --candidate-index <n> Download the zero-based matching search candidate.");
            Console.WriteLine("  --candidate-cache <path> Reuse a short-lived same-title candidate list cache.");
            Console.WriteLine("  --title-only  Match candidates by title without requiring the artist.");
            Console.WriteLine("  --list        Search and print tab-separated results without downloading.");
            Console.WriteLine("  --self-test   Run deterministic matching regression tests.");
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
            public string ManifestPath;
            public string CandidateCachePath;
            public string Sources = "lrclib,qq1";
            public bool CachedOnly;
            public bool SearchOnly;
            public bool ListOnly;
            public bool SelfTest;
            public bool TitleOnly;
            public int CandidateIndex = -1;
            public bool Help;
        }

        private sealed class LyricsRecord
        {
            public LyricsRecord() { }
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

        private sealed class CandidateCache
        {
            public CandidateCache() { }
            public string Title;
            public string Artist;
            public string Album;
            public string Sources;
            public int DurationSeconds;
            public bool TitleOnly;
            public long CreatedUtcTicks;
            public List<LyricsRecord> Records;
        }
    }
}









