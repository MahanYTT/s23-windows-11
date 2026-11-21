Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawDiskReader {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFileW(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    static string GetLfnPart(byte[] buf, int off) {
        byte[] chars = new byte[26];
        Array.Copy(buf, off + 1, chars, 0, 10);
        Array.Copy(buf, off + 14, chars, 10, 12);
        Array.Copy(buf, off + 28, chars, 22, 4);
        string s = Encoding.Unicode.GetString(chars);
        int nul = s.IndexOf('\0');
        return nul >= 0 ? s.Substring(0, nul) : s;
    }

    static byte[] ReadChain(FileStream fs, ushort[] fat, long dataOff, int clusterBytes, ushort startClus) {
        MemoryStream ms = new MemoryStream();
        byte[] buf = new byte[clusterBytes];
        ushort c = startClus;
        int guard = 0;
        while (c >= 2 && c < 0xFFF8 && guard++ < 4096) {
            fs.Position = dataOff + (long)(c - 2) * clusterBytes;
            fs.Read(buf, 0, clusterBytes);
            ms.Write(buf, 0, clusterBytes);
            c = fat[c];
        }
        return ms.ToArray();
    }

    static void ParseDir(byte[] dir, string prefix, bool filter) {
        List<string> lfnParts = new List<string>();
        for (int i = 0; i < dir.Length; i += 32) {
            if (dir[i] == 0) break;
            if (dir[i] == 0xE5) { lfnParts.Clear(); continue; }
            if (dir[i + 11] == 0x0F) {
                lfnParts.Insert(0, GetLfnPart(dir, i));
                continue;
            }
            string sname = Encoding.ASCII.GetString(dir, i, 8).Trim();
            string sext = Encoding.ASCII.GetString(dir, i + 8, 3).Trim();
            string shortFull = sext.Length > 0 ? (sname + "." + sext) : sname;
            string longFull = lfnParts.Count > 0 ? string.Concat(lfnParts.ToArray()) : shortFull;
            lfnParts.Clear();
            byte attr = dir[i + 11];
            ushort clus = BitConverter.ToUInt16(dir, i + 26);
            uint sz = BitConverter.ToUInt32(dir, i + 28);
            if (!filter ||
                longFull.IndexOf("wlan", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("amss", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("m3", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("bd", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("reg", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("data", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("qca", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("wcn", StringComparison.OrdinalIgnoreCase) >= 0 ||
                longFull.IndexOf("hsp", StringComparison.OrdinalIgnoreCase) >= 0) {
                Console.WriteLine("  " + prefix + "/" + longFull + " (" + shortFull + ") attr=0x" + attr.ToString("X2") + " clus=" + clus + " size=" + sz);
            }
        }
    }

    public static void ListFat16(FileStream fs, string partName, long partOff) {
        byte[] bs = new byte[4096];
        fs.Position = partOff;
        fs.Read(bs, 0, 4096);
        ushort bps = BitConverter.ToUInt16(bs, 11);
        byte spc = bs[13];
        ushort rsvd = BitConverter.ToUInt16(bs, 14);
        byte nfats = bs[16];
        ushort rootEnts = BitConverter.ToUInt16(bs, 17);
        ushort spf = BitConverter.ToUInt16(bs, 22);
        Console.WriteLine("=== " + partName + " @ 0x" + partOff.ToString("X") + " ===");
        if (bps != 512 && bps != 4096) return;
        long fatOff = partOff + (long)rsvd * bps;
        int fatBytes = spf * bps;
        byte[] fatRaw = new byte[fatBytes];
        fs.Position = fatOff;
        fs.Read(fatRaw, 0, fatBytes);
        ushort[] fat = new ushort[fatBytes / 2];
        Buffer.BlockCopy(fatRaw, 0, fat, 0, fatBytes);

        long rootOff = partOff + (long)(rsvd + nfats * spf) * bps;
        int rootBytes = ((rootEnts * 32 + bps - 1) / bps) * bps;
        long dataOff = rootOff + rootBytes;
        int clusterBytes = bps * spc;

        byte[] root = new byte[rootBytes];
        fs.Position = rootOff;
        fs.Read(root, 0, rootBytes);
        ParseDir(root, "", false);

        byte[] imgDir = ReadChain(fs, fat, dataOff, clusterBytes, 2);
        Console.WriteLine("  --- /image (" + imgDir.Length + " bytes) ---");
        ParseDir(imgDir, "/image", false);
    }
}
'@
$h = [RawDiskReader]::CreateFileW('\\.\PhysicalDrive0', 0x120089, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
$fs = New-Object IO.FileStream($h, [IO.FileAccess]::Read, 4096)
[RawDiskReader]::ListFat16($fs, 'apnhlos', 0x0F1F0000L)
[RawDiskReader]::ListFat16($fs, 'modem', 0x1C800000L)
$fs.Close()
$h.Close()
