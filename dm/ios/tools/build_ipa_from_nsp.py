#!/usr/bin/env python3
# 从 NSP 一路出未签名 IPA：解包（nx_extract）→ 改写代码模块（patch_nso.py）→ 拆出着色器并与仓内表核对（shader_split.py）→ iOS 着色器库（dmsh_build.py）→ build_ipa.sh。
# 在 Spyder 里打开本文件按 F5 运行（也可 python3 build_ipa_from_nsp.py）；下面「设置」留空的两项运行时会问。
# 只用三样东西：本仓库、放 NSP 的文件夹（基础版 v0 + 更新 1.2.1 都要：着色器表是两版的并集；DLC 跳过，本移植不读追加内容）、prod.keys。
# 所有中间产物都在 NSP 文件夹里的 smbw_build/：extract/（解出的游戏文件，出包后默认删掉）、tools/、patched_*/、shader/（着色器包与缓存，留着下次出包快）、logs/（每步的完整输出）、tmp/、ipa/。
# 着色器翻译器用仓内的 dm/gfx/tools/dmsh_xlate_bin（与 dmsh_chain.lock 指纹一致的那份；重编出的 dll 指纹不同，dmsh_build 会拒绝）。
# 运行本文件的 Python 要有 lz4、capstone、fonttools；另需 Xcode、xcodegen、.NET SDK 10、zstd、spirv-cross。首次约 35 分钟（大头是着色器库），再跑约 10 分钟（着色器库走缓存）。密钥值一律不打印。
# Unsigned IPA straight from the NSPs: extract (nx_extract) → rewrite the code modules (patch_nso.py) → split out the shaders and check them against the in-repo tables (shader_split.py) → iOS shader libraries (dmsh_build.py) → build_ipa.sh.
# Open this file in Spyder and press F5 (or run python3 build_ipa_from_nsp.py); the two settings below that are left empty are asked for when it runs.
# Uses only three things: this repository, the folder holding the NSPs (both the base game v0 and update 1.2.1 are required: the shader tables are the union of both versions; DLC is skipped, this port does not load add-on content), and prod.keys.
# Every intermediate file is under smbw_build/ in the NSP folder: extract/ (the extracted game files, deleted after the IPA is built by default), tools/, patched_*/, shader/ (shader package and caches, kept so the next build is fast), logs/ (each step's full output), tmp/, ipa/.
# The shader translator is the in-repo dm/gfx/tools/dmsh_xlate_bin (the build whose fingerprints match dmsh_chain.lock; a rebuilt dll has different fingerprints and dmsh_build refuses it).
# The Python running this file needs lz4, capstone and fonttools; also needed: Xcode, xcodegen, .NET SDK 10, zstd, spirv-cross. The first run takes about 35 minutes (mostly the shader libraries); later runs take about 10 (the shader libraries come from the cache). Key values are never printed.
import collections, ctypes, getpass, os, re, shutil, struct, subprocess, sys, threading, time
from pathlib import Path

# ---- 设置 | Settings ----
NSP_DIR = ''                            # 放 NSP 的文件夹（留空 = 运行时问） | folder holding the NSPs (empty = ask at run time)
PROD_KEYS = ''                          # prod.keys 的路径（留空 = 运行时问） | path of prod.keys (empty = ask at run time)
PACKAGE = 'v327680'                     # 打进 IPA 的版本：'v327680'（1.2.1）或 'v0' | version packed into the IPA: 'v327680' (1.2.1) or 'v0'
OUT_IPA = '~/Desktop/SMBWMobile.ipa'    # 输出的 IPA | output IPA
KEEP_EXTRACTED = False                  # 出包后保留解出的游戏文件（下次省去解包，约 3.6 GB） | keep the extracted game files after the IPA is built (saves extraction next time, about 3.6 GB)
DM_ENV_FILE = ''                        # 可选：诊断包的环境开关文件（每行 KEY=VALUE） | optional: environment switches for a diagnostic build (one KEY=VALUE per line)

TITLE = 0x010015100B514000              # Super Mario Bros. Wonder 的标题号 | title ID of Super Mario Bros. Wonder
# 本移植认得的构建（main / sdk 构建号，见 README「Update 1.2.1」）；别的构建上写死的地址不成立，直接停 | Builds this port knows (main / sdk build IDs, see README "Update 1.2.1"); fixed addresses do not hold on other builds, so stop
BUILDS = {'v0': {'main': 'cd6e42aee7934f4d8393cd43893ae25e', 'sdk': '3053b22935e32ddf15ef81997603e5c2'},
          'v327680': {'main': 'ff773e90972d544eb79406eaa65396d5', 'sdk': 'e4bd58cac28cf13a08fb417aa013cdcb'}}
BEAT = 60                               # quiet 步骤的进度间隔（秒） | progress interval of quiet steps (seconds)
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin', '/usr/local/share/dotnet', os.path.expanduser('~/.dotnet')]   # Spyder 等图形程序拿不到 shell 的 PATH | GUI apps such as Spyder do not get the shell's PATH


class BuildError(Exception):
    pass


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def ask(prompt):
    s = input(prompt).strip().strip('"').strip("'").replace('\\ ', ' ')   # 拖进来的路径带引号或转义空格 | dragged-in paths come quoted or with escaped spaces
    return os.path.expanduser(s)


def run(cmd, log, env=None, cwd=None, quiet=False):
    """跑一个子进程，全部输出写进 log；非零退出即停。quiet：控制台每分钟报一次进度，结束时只打末尾几行（着色器出包有上万行编译器警告）
    Run a subprocess with all its output written to log; stop on a non-zero exit. quiet: report progress to the console once a minute and print only the last lines at the end (the shader build emits tens of thousands of compiler warnings)"""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    tail, n, done = collections.deque(maxlen=40), [0], threading.Event()
    def beat():   # 子进程不出声时也报（走缓存的着色器出包几分钟没有输出） | report even while the subprocess is silent (a cached shader build prints nothing for minutes)
        t0 = time.time()
        while not done.wait(BEAT): say(f'……仍在进行，{(time.time() - t0) / 60:.0f} 分钟 | still running, {(time.time() - t0) / 60:.0f} min')
    p = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1)
    if quiet: threading.Thread(target=beat, daemon=True).start()
    try:
        with open(log, 'w') as f:
            for line in p.stdout:
                f.write(line); tail.append(line); n[0] += 1
                if not quiet: print(line, end='', flush=True)
        p.wait()
    except KeyboardInterrupt:
        p.terminate(); p.wait(); raise
    finally:
        done.set()
    if quiet: print(''.join(list(tail)[-40 if p.returncode else -12:]), end='', flush=True)
    if p.returncode:
        tool = Path(cmd[1] if cmd[0] in (sys.executable, 'bash') else cmd[0]).name
        raise BuildError(f'{tool} 退出码 | exit code {p.returncode}（完整输出 | full output: {log}）')


def build_id(nso):
    with open(nso, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def rmtree(p):
    if p.is_symlink() or p.is_file(): p.unlink()
    elif p.exists(): shutil.rmtree(p)


def swap_in(tmp, dst):   # 新产物整个换上（失败时旧的不受影响） | swap the new output in as a whole (the old one is untouched on failure)
    rmtree(dst); tmp.rename(dst)


def free_gb(p):
    while not p.exists(): p = p.parent
    return shutil.disk_usage(p).free / 2**30


# ---- NSP 识别 | NSP identification ----
def pfs0_names(path):   # NSP = PFS0：列出里面的文件名 | NSP = PFS0: list the file names inside
    with open(path, 'rb') as f:
        h = f.read(16)
        if h[:4] != b'PFS0': return []
        n, st = struct.unpack_from('<II', h, 4)
        ent, strs = f.read(n * 0x18), f.read(st)
    return [strs[o:strs.index(b'\0', o)].decode(errors='replace') for o in (struct.unpack_from('<I', ent, i * 0x18 + 0x10)[0] for i in range(n))]


def title_of(path):   # 文件名里的 [标题号]，没有就取票据名（权利号前 16 位 = 标题号） | [title ID] in the file name, else the ticket name (the first 16 digits of the rights ID = title ID)
    m = re.search(r'\[([0-9A-Fa-f]{16})\]', path.name)
    if m: return int(m.group(1), 16)
    for n in pfs0_names(path):
        if re.match(r'[0-9A-Fa-f]{32}\.tik$', n): return int(n[:16], 16)
    return None


def version_of(path):
    m = re.search(r'\[v(\d+)\]', path.name)
    return int(m.group(1)) if m else None


def find_nsps(nsp_dir):
    base, updates, unknown = None, [], []
    for p in sorted(nsp_dir.glob('*.nsp')):
        tid = title_of(p)
        if tid is None: unknown.append(p); continue
        low = tid & 0xFFF
        kind, app = ('base', tid) if low == 0 else ('update', tid & ~0xFFF) if low == 0x800 else ('dlc', (tid & ~0xFFF) - 0x1000)
        if app != TITLE: say(f'忽略（不是 Super Mario Bros. Wonder）| ignored (not Super Mario Bros. Wonder): {p.name}'); continue
        if kind == 'dlc': say(f'跳过 DLC（本移植不读追加内容）| skipping DLC (this port does not load add-on content): {p.name}'); continue
        if kind == 'base': base = p
        else: updates.append(p)
    if base is None and len(unknown) == 1: base = unknown[0]   # 卡带转出的基础版可能既无标签也无票据；构建号随后核对 | a base game converted from a cartridge may have neither tags nor a ticket; build IDs are checked afterwards
    if base is None: raise BuildError(f'{nsp_dir} 里没找到基础版 NSP（标题号 {TITLE:016X}）| no base game NSP found (title ID {TITLE:016X})')
    upd = [u for u in updates if version_of(u) == 327680] or (updates if len(updates) == 1 else [])
    if not upd: raise BuildError('没找到 1.2.1 更新 NSP（v327680）：着色器表是 v0 + 1.2.1 的并集，两版都要 | no update 1.2.1 NSP (v327680) found: the shader tables are the union of v0 + 1.2.1, both are required')
    say(f'基础版 | base: {base.name}'); say(f'更新 | update: {upd[0].name}')
    return base, upd[0]


def can_clone(d):   # 该卷能否做 APFS 克隆（nx_extract 用它省空间，不支持时整份写出） | whether this volume supports APFS clones (nx_extract uses them to save space; without them it writes full copies)
    a, b = d / '.clone_test', d / '.clone_test2'
    try:
        a.write_bytes(b'x'); rmtree(b)
        return ctypes.CDLL(None, use_errno=True).clonefile(str(a).encode(), str(b).encode(), 0) == 0
    finally:
        rmtree(a); rmtree(b)


# ---- 各步 | Steps ----
def check_prereqs(env, xlate):
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools') if not _importable(m)]
    if missing: raise BuildError(f'当前 Python（{sys.executable}）缺模块 | the current Python is missing modules: {", ".join(missing)}（pip install lz4 capstone fonttools）')
    tools = {'xcodebuild': 'Xcode', 'xcodegen': 'brew install xcodegen', 'dotnet': '.NET SDK 10', 'zstd': 'brew install zstd', 'spirv-cross': 'brew install spirv-cross', 'zip': 'zip', 'make': 'Xcode'}
    gone = [f'{t}（{how}）' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if gone: raise BuildError('找不到工具 | tools not found: ' + '，'.join(gone))
    if not xlate.exists(): raise BuildError(f'仓内没有着色器翻译器 | the shader translator is missing from the repository: {xlate}')


def _importable(m):
    try: __import__(m); return True
    except ImportError: return False


def extract(nx, keys, ex, name, base, upd, cmp_dir, logs):
    dst = ex / name
    if (dst / 'ExeFS' / 'main').exists() and (dst / 'RomFS' / 'System' / 'Resource').is_dir():
        say(f'沿用已解出的 {name} | reusing the extracted {name}: {dst}')
    else:
        tmp = ex / (name + '.partial'); rmtree(tmp)
        say(f'解包 {name} | extracting {name} → {dst}')
        run([nx, keys, base, upd or '-', tmp] + ([cmp_dir] if cmp_dir else []), logs / f'extract_{name}.log')
        check_build(tmp, name); swap_in(tmp, dst)
    check_build(dst, name)
    return dst


def check_build(d, name):
    for m, want in BUILDS[name].items():
        got = build_id(d / 'ExeFS' / m)
        if got != want: raise BuildError(f'{m} 构建号 {got} 不是本移植认得的 {name}（{want}）| {m} build ID {got} is not the {name} this port knows ({want})')


def rewrite(py, repo, exe_dir, out, env, logs):
    tmp = out.with_name(out.name + '.partial'); rmtree(tmp); tmp.mkdir(parents=True)
    for m in ('main', 'sdk'):
        say(f'改写 {m} | rewriting {m}')
        run([py, repo / 'dm/tools/patch_nso.py', exe_dir / m, tmp, m], logs / f'rewrite_{m}.log', env=dict(env, DM_RULESET='native'))
    swap_in(tmp, out)


def split_shaders(py, repo, roms, mains, pkg, env, logs):
    tmp = pkg.with_name(pkg.name + '.partial'); rmtree(tmp); (tmp / 'tables').mkdir(parents=True)
    say('拆出着色器 | splitting out the shaders')
    names = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv')
    run([py, repo / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), tmp / 'pkg'] + [tmp / 'tables' / n for n in names], logs / 'shader_split.log', env=env)
    # 表与仓内逐字节相同 = 游戏文件与仓库建表用的是同一份；不同就别往下出包（仓内表不改） | Tables byte-identical to the repo's = the same game files the repo's tables were built from; if not, stop (the repo's tables are left alone)
    diff = [n for n in names if (tmp / 'tables' / n).read_bytes() != (repo / 'dm/gfx/nvn' / n).read_bytes()]
    if diff: raise BuildError(f'拆出的着色器表与仓内不同 | the split shader tables differ from the repository\'s: {", ".join(diff)}（游戏文件不是 v0 + 1.2.1？| game files not v0 + 1.2.1?）')
    swap_in(tmp / 'pkg', pkg); rmtree(tmp)


def build_shaders(py, repo, sh, xlate, env, logs):
    say('出 iOS 着色器库（首次约 30 分钟，之后走缓存约 7 分钟）| building the iOS shader libraries (about 30 minutes the first time, about 7 with the cache)')
    ios = sh / 'ios'
    run([py, repo / 'dm/gfx/tools/dmsh_build.py', sh / 'pkg', sh / 'cache'], logs / 'dmsh_build.log', env=dict(env, XLATE=str(xlate), IOSOUT=str(ios)), quiet=True)
    chain = re.search(r'#define DMSH_CHAIN "(.*)"', (repo / 'dm/gfx/nvn/dmsh_chain.h').read_text()).group(1)
    libs = ios / f'b-{chain}'
    n = sum(1 for _ in libs.glob('*_b.metallib')) if libs.is_dir() else 0
    if n == 0: raise BuildError(f'没出 iOS 着色器库 | no iOS shader libraries in {libs}')
    say(f'iOS 着色器库 {n} 个 | {n} iOS shader libraries ({libs})')
    return ios


def main():
    here = Path(globals().get('__file__') or sys.argv[0]).resolve()
    repo = here.parents[3]   # dm/ios/tools/本文件 | dm/ios/tools/this file
    if not (repo / 'dm/tools/nx_extract/Program.cs').exists(): raise BuildError(f'从 {here} 推不出仓库根目录 | cannot find the repository root from {here}')
    nsp_dir = Path(os.path.expanduser(NSP_DIR) or ask('NSP 文件夹 | NSP folder: ')).resolve()
    keys = Path(os.path.expanduser(PROD_KEYS) or ask('prod.keys 路径 | path of prod.keys: ')).resolve()
    if not nsp_dir.is_dir(): raise BuildError(f'不是文件夹 | not a folder: {nsp_dir}')
    if not keys.is_file(): raise BuildError(f'没有这个文件 | no such file: {keys}')
    if PACKAGE not in BUILDS: raise BuildError(f"PACKAGE 只能是 | PACKAGE must be one of: {', '.join(BUILDS)}")
    out_ipa = Path(os.path.expanduser(OUT_IPA))
    work = nsp_dir / 'smbw_build'
    xlate = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    # DM_ROOT / WORK 指到 smbw_build：各工具的缺省路径与临时目录都不出它 | DM_ROOT / WORK point at smbw_build: the tools' default paths and temporary directories all stay inside it
    env = dict(os.environ, PATH=os.pathsep.join(TOOL_PATHS + [os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')]), DM_ROOT=str(work), WORK=str(work / 'tmp'))
    env.setdefault('USER', getpass.getuser())   # xcodegen 没有 USER 就失败（Couldn't find current username） | xcodegen fails without USER (Couldn't find current username)
    logs = work / 'logs'
    py = sys.executable
    check_prereqs(env, xlate)
    base, upd = find_nsps(nsp_dir)

    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    if free_gb(work) < 12 and not (ex / 'v327680').exists(): raise BuildError(f'{work} 所在的卷剩余不足 12 GB | less than 12 GB free on the volume of {work}')
    nx = work / 'tools/nx_extract/nx_extract'
    say('编译 nx_extract | building nx_extract')
    run(['dotnet', 'build', repo / 'dm/tools/nx_extract', '-c', 'Release', '-o', nx.parent, '-v', 'quiet', '-nologo'], logs / 'nx_extract_build.log', env=env)
    v0 = extract(nx, keys, ex, 'v0', base, None, None, logs)
    v1 = extract(nx, keys, ex, 'v327680', base, upd, v0 / 'RomFS' if can_clone(ex) else None, logs)
    game = v1 if PACKAGE == 'v327680' else v0

    patched = work / f'patched_{PACKAGE}'
    rewrite(py, repo, game / 'ExeFS', patched, env, logs)
    sh = work / 'shader'
    split_shaders(py, repo, [v0 / 'RomFS', v1 / 'RomFS'], [v0 / 'ExeFS/main', v1 / 'ExeFS/main'], sh / 'pkg', env, logs)
    ios = build_shaders(py, repo, sh, xlate, env, logs)

    if free_gb(out_ipa.parent) < 5: raise BuildError(f'{out_ipa.parent} 所在的卷剩余不足 5 GB | less than 5 GB free on the volume of {out_ipa.parent}')
    say(f'出 IPA | building the IPA → {out_ipa}')
    ipa_env = dict(env, W=str(work / 'ipa'), PATCHED=str(patched), ROMFS=str(game / 'RomFS'), MSL_IOS=str(ios), FONT_PY=py)
    if DM_ENV_FILE: ipa_env['DM_ENV_FILE'] = os.path.expanduser(DM_ENV_FILE)
    run(['bash', repo / 'dm/ios/tools/build_ipa.sh', out_ipa], logs / 'build_ipa.log', env=ipa_env)
    if not out_ipa.is_file(): raise BuildError(f'没出 IPA | no IPA at {out_ipa}')
    rmtree(work / 'tmp')
    if not KEEP_EXTRACTED:
        say(f'删掉解出的游戏文件 | deleting the extracted game files: {ex}'); rmtree(ex)
    say(f'完成 | done: {out_ipa}（{out_ipa.stat().st_size / 2**30:.1f} GB）')


if __name__ == '__main__':
    try:
        main()
    except BuildError as e:
        print(f'【红】{e}', flush=True)
    except KeyboardInterrupt:
        print('【红】中断 | interrupted', flush=True)
