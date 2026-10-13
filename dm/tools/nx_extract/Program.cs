// NSP 解包：nx_extract <prod.keys> <基础 NSP> <更新 NSP 或 -> <输出目录> [比对用的基础 RomFS 目录]
// 输出 <输出目录>/ExeFS 与 <输出目录>/RomFS（有更新包时是合并后的 RomFS）；给了基础 RomFS 目录时，内容与之相同的文件做成 APFS 克隆（不占新空间）。
// 标题密钥取自 NSP 里的通用票据（.tik），只需 prod.keys；密钥值一律不打印。
// NSP extraction: nx_extract <prod.keys> <base NSP> <update NSP or -> <output dir> [base RomFS dir to compare against]
// Writes <output dir>/ExeFS and <output dir>/RomFS (the merged RomFS when there is an update); with a base RomFS dir, files whose content matches it become APFS clones (no new space).
// Title keys come from the common tickets (.tik) inside the NSPs, so only prod.keys is needed; key values are never printed.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Spl;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;
using Path = System.IO.Path;

static class P {
    [DllImport("libc", SetLastError = true)] static extern int clonefile(string src, string dst, uint flags);

    static int Main(string[] a) {
        if (a.Length < 4) { Console.Error.WriteLine("用法 | usage: nx_extract <prod.keys> <base.nsp> <update.nsp|-> <out dir> [base RomFS dir]"); return 2; }
        KeySet ks = ExternalKeyReader.ReadKeyFile(a[0]);
        var basePkg = OpenNsp(ks, a[1]); var updPkg = a[2] == "-" ? null : OpenNsp(ks, a[2]);
        Nca baseProg = Pick(basePkg, NcaContentType.Program) ?? throw new Exception("基础 NSP 里没有程序 NCA | no Program NCA in the base NSP");
        Nca updProg = updPkg == null ? null : Pick(updPkg, NcaContentType.Program) ?? throw new Exception("更新 NSP 里没有程序 NCA | no Program NCA in the update NSP");
        Nca ctrl = Pick(updPkg ?? basePkg, NcaContentType.Control);
        Console.WriteLine($"标题 | title {baseProg.Header.TitleId:x16}" + (updProg != null ? $"，更新 | update {updProg.Header.TitleId:x16}" : ""));
        if (ctrl != null) Console.WriteLine($"显示版本 | display version {DisplayVersion(ctrl)}");

        string outDir = a[3], cmpDir = a.Length > 4 ? a[4] : null;
        // 代码区：更新包的 ExeFS 是完整的，叠在基础上打开也一样 | Code section: the update's ExeFS is complete; opening it layered over the base gives the same result
        IFileSystem exefs = updProg != null ? baseProg.OpenFileSystemWithPatch(updProg, NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid) : baseProg.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid);
        Copy(exefs, Path.Combine(outDir, "ExeFS"), null);
        foreach (string m in new[] { "main", "sdk" }) {
            string p = Path.Combine(outDir, "ExeFS", m);
            if (File.Exists(p)) { byte[] h = new byte[0x20]; using (var f = File.OpenRead(p)) { f.Seek(0x40, SeekOrigin.Begin); f.ReadExactly(h); } Console.WriteLine($"{m} 构建号 | build id {Convert.ToHexString(h, 0, 16).ToLowerInvariant()}"); }
        }
        IStorage romfs = updProg != null ? baseProg.OpenStorageWithPatch(updProg, NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid) : baseProg.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        Copy(new RomFsFileSystem(romfs), Path.Combine(outDir, "RomFS"), cmpDir);
        return 0;
    }

    // 打开 NSP（PFS0），导入其中的通用票据，列出所有 NCA | Open an NSP (PFS0), import its common tickets, list all NCAs
    static List<Nca> OpenNsp(KeySet ks, string path) {
        var pfs = new PartitionFileSystem();
        pfs.Initialize(new LocalStorage(path, FileAccess.Read)).ThrowIfFailure();
        var ncas = new List<Nca>();
        foreach (DirectoryEntryEx e in pfs.EnumerateEntries("*", SearchOptions.Default)) {
            using var f = new UniqueRef<IFile>();
            pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            IStorage st = f.Release().AsStorage();
            if (e.Name.EndsWith(".tik", StringComparison.OrdinalIgnoreCase)) {
                byte[] t = new byte[0x2c0]; st.Read(0, t).ThrowIfFailure();
                if (t[0x281] != 0) { Console.Error.WriteLine($"【红】{e.Name} 不是通用票据，标题密钥要另给 | is not a common ticket; supply the title key separately"); continue; }
                ks.ExternalKeySet.Add(new RightsId(t.AsSpan(0x2a0, 0x10)), new AccessKey(t.AsSpan(0x180, 0x10))).ThrowIfFailure();
            } else if (e.Name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase)) ncas.Add(new Nca(ks, st));
        }
        return ncas;
    }
    static Nca Pick(List<Nca> l, NcaContentType t) { foreach (Nca n in l) if (n.Header.ContentType == t) return n; return null; }

    static string DisplayVersion(Nca ctrl) {
        IFileSystem fs = ctrl.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        using var f = new UniqueRef<IFile>();
        fs.OpenFile(ref f.Ref, "/control.nacp".ToU8Span(), OpenMode.Read).ThrowIfFailure();
        byte[] v = new byte[0x10]; f.Get.Read(out _, 0x3060, v, ReadOption.None).ThrowIfFailure();   // NACP DisplayVersion @0x3060
        return System.Text.Encoding.ASCII.GetString(v).TrimEnd('\0');
    }

    // 逐文件拷出；与比对目录同路径、同内容的文件改做 APFS 克隆 | Copy out file by file; files identical to the same path in the compare dir become APFS clones instead
    static void Copy(IFileSystem fs, string dst, string cmpDir) {
        long n = 0, cloned = 0, bytes = 0, written = 0; byte[] buf = new byte[4 << 20], cb = new byte[4 << 20];
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            string rel = e.FullPath.TrimStart('/'), op = Path.Combine(dst, rel);
            if (e.Type == DirectoryEntryType.Directory) { Directory.CreateDirectory(op); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(op)); n++; bytes += e.Size;
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            string cp = cmpDir == null ? null : Path.Combine(cmpDir, rel);
            if (cp != null && File.Exists(cp) && new FileInfo(cp).Length == e.Size && Same(f.Get, cp, e.Size, buf, cb)) {
                if (File.Exists(op)) File.Delete(op);
                if (clonefile(cp, op, 0) != 0) throw new IOException($"clonefile {cp} → {op} 失败 | failed: errno {Marshal.GetLastWin32Error()}");
                cloned++; continue;
            }
            using var o = File.Create(op);
            for (long off = 0; off < e.Size;) {
                f.Get.Read(out long got, off, buf.AsSpan(0, (int)Math.Min(buf.Length, e.Size - off)), ReadOption.None).ThrowIfFailure();
                if (got <= 0) throw new IOException($"读不出 | cannot read {e.FullPath} @{off}");
                o.Write(buf, 0, (int)got); off += got;
            }
            written += e.Size;
        }
        Console.WriteLine($"{dst}：{n} 个文件 {bytes / 1048576.0:F1} MB，其中 {cloned} 个与比对目录相同（克隆），新写 {written / 1048576.0:F1} MB | {n} files {bytes / 1048576.0:F1} MB, {cloned} identical to the compare dir (cloned), {written / 1048576.0:F1} MB newly written");
    }
    static bool Same(IFile f, string path, long size, byte[] buf, byte[] cb) {
        using var c = File.OpenRead(path);
        for (long off = 0; off < size;) {
            int want = (int)Math.Min(buf.Length, size - off);
            f.Read(out long got, off, buf.AsSpan(0, want), ReadOption.None).ThrowIfFailure();
            c.ReadExactly(cb, 0, (int)got);
            if (!buf.AsSpan(0, (int)got).SequenceEqual(cb.AsSpan(0, (int)got))) return false;
            off += got;
        }
        return true;
    }
}
