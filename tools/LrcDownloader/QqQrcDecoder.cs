using System;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace LrcDownloader
{
    internal static class QqQrcDecoder
    {
        private static readonly byte[] QqKey = Encoding.ASCII.GetBytes("!@#)(*$%123ZXC!@!@#)(NHL");

        public static bool LooksEncryptedQrc(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return false;
            value = Regex.Replace(value.Trim(), @"\s+", string.Empty);
            return value.Length >= 16 && (value.Length % 16) == 0 && Regex.IsMatch(value, @"\A[0-9a-fA-F]+\z");
        }

        public static string DecryptHexToText(string encryptedHex)
        {
            var encryptedBytes = HexStringToByteArray(Regex.Replace(encryptedHex.Trim(), @"\s+", string.Empty));
            if (encryptedBytes.Length == 0 || (encryptedBytes.Length % 8) != 0) return string.Empty;

            var decryptedBytes = new byte[encryptedBytes.Length];
            var schedule = CreateSchedule();
            QqDesHelper.TripleDESKeySetup(QqKey, schedule, QqDesHelper.DECRYPT);

            for (var offset = 0; offset < encryptedBytes.Length; offset += 8)
            {
                var block = new byte[8];
                Buffer.BlockCopy(encryptedBytes, offset, block, 0, 8);
                var output = new byte[8];
                QqDesHelper.TripleDESCrypt(block, output, schedule);
                Buffer.BlockCopy(output, 0, decryptedBytes, offset, 8);
            }

            var inflated = TryInflate(decryptedBytes);
            return inflated.Length == 0 ? string.Empty : Encoding.UTF8.GetString(inflated);
        }

        private static byte[][][] CreateSchedule()
        {
            var schedule = new byte[3][][];
            for (var i = 0; i < schedule.Length; i++)
            {
                schedule[i] = new byte[16][];
                for (var j = 0; j < schedule[i].Length; j++) schedule[i][j] = new byte[6];
            }
            return schedule;
        }

        private static byte[] HexStringToByteArray(string hexString)
        {
            var bytes = new byte[hexString.Length / 2];
            for (var i = 0; i < bytes.Length; i++)
            {
                bytes[i] = byte.Parse(hexString.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            }
            return bytes;
        }

        private static byte[] TryInflate(byte[] data)
        {
            var result = Inflate(data, 0);
            if (result.Length > 0) return result;

            if (data.Length > 2 && data[0] == 0x78)
            {
                result = Inflate(data, 2);
                if (result.Length > 0) return result;
            }

            return Array.Empty<byte>();
        }

        private static byte[] Inflate(byte[] data, int offset)
        {
            try
            {
                using (var input = new MemoryStream(data, offset, data.Length - offset))
                using (var deflate = new DeflateStream(input, CompressionMode.Decompress))
                using (var output = new MemoryStream())
                {
                    deflate.CopyTo(output);
                    return output.ToArray();
                }
            }
            catch
            {
                return Array.Empty<byte>();
            }
        }
    }
}
