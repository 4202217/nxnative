# 游戏档案

本移植认得的每个游戏都有一个目录 `dm/games/<名>/`，以基础版的标题号命名：马力欧是 `010015100B514000`，Engage 是 `0100A6301214E000`。目录里有：

- **`game.tsv`**：这个游戏的常数，每行 `键<TAB>值`，注释独占一行。
- **着色器表**（`shader_split.py` 写出）：`shader_map.tsv`（含 `.conflicts.tsv`）、`shader_facts.tsv`、`shader_pairs.tsv`。
- **`c1_full.tsv`**：每次出着色器库后由 `c1_guard.py` 重生成。
- **`pso_manifest.tsv`**：预热种子清单。
- **`bf_want.tsv`**。
- **`dmsh_chain.h`**：翻译链版本，由 `dmsh_chain.py --header` 写出。

和游戏有关的都放这里。代码读档案，不写死某个游戏。

| 谁 | 用什么选档案 | 读什么 |
|---|---|---|
| 着色器工具（`shader_split.py`、`dmsh_chain.py`、`dmsh_build.py`、`dmsh_facts.py`） | `DM_GAME=<名>` | 各表，`nvn_*`、`shader_*` 键 |
| 宿主（`make -C dm/host`、`dm/gfx/tests`） | `GAME=<名>` | `gen/<名>/game_profile.h`（`game.py header` 由 `game.tsv` 生成） |
| `dm/ios/tools/build_ipa.sh` | `GAME=<名>` | App 名、包名、IPA 名、代码模块、RomFS 认定路径、各表 |
| `scripts/build_ipa_from_nsp.py` | NSP 的标题号 | 以上全部。没有对应档案时只写粗查报告 |

都没有缺省游戏：不给就停下并列出档案。查档案：
- `python3 dm/games/game.py list` 列出全部档案。
- `python3 dm/games/game.py <名> <键>` 打印一个键。

## 键

| 键 | 含义 |
|---|---|
| `title_id`、`program_id` | 身份。`program_id` 是 `svcGetInfo(ProgramId)` 报给游戏的值。游戏名不存：NSP 脚本从 NSP 的控制数据（NACP）读 |
| `name` | 游戏名，只给 `game.py list`、报错信息和 Mac 窗口标题当标签。iOS App 名取基础版 NSP 控制数据里的游戏名，包名是 `org.nxnative.<标题号小写>`；NSP 脚本的 `APP_NAME` / `BUNDLE_ID`、`build_ipa.sh` 的 `DM_DISPLAY_NAME` / `DM_BUNDLE_ID` 可改 |
| `ipa`、`app_category` | iOS App：缺省 IPA 文件名与 App Store 分类 |
| `build <版本> <显示版本> <模块>=<构建号>…` | 每个已知版本一行。NSP 脚本只认这些构建；宿主按 `main` 的构建号报显示版本 |
| `package` | 缺省打进 IPA 的版本 |
| `shader_versions` | 着色器表是这几个版本的并集（基础版在前）。出包时这些版本都要有 |
| `modules` | 改写并装载的代码模块，按装载顺序。`rtld` 由宿主代替 |
| `romfs_marker` | RomFS 里必须有的路径 |
| `zip_store` | IPA 里不再压缩的扩展名 |
| `mem_reserve`、`mem_alias`、`mem_stack`、`app_pool` | 客体地址空间分区与应用内存池（`hle_svc.c`） |
| `present_interval` | `game`（缺省）：按游戏设的出图间隔出帧，同 Switch（按间隔 2 出的一帧在屏上留两个显示节拍；游戏连续一秒都要 2 才换 30 帧目标，iOS 上显示节拍随之 30 Hz；马力欧 1.2.1 在慢一帧之后只给下一帧设 2；Engage 在 Switch 上设 2，2.0.0 `main` 的改写剪口让它设 1，见 `0100A6301214E000/game.tsv`）。`display`：不管游戏设多少，每个显示节拍一帧——只适合逻辑不按帧计数的游戏（D2R 的逻辑按自己的 25 Hz 走）。`DM_PRESENT_INTERVAL=game` / `display` 可覆盖，也可写在 `Documents/env.txt` |
| `core_preempt` | `on` / `off`（缺省）：近似 Horizon 的按核抢占。一次 SVC 叫醒了钉在调用线程本核、优先级更高的线程，调用线程就等这些线程再次阻塞（最多 5 ms，`hle_thread.c`）。银河战士 生存恐惧的影片播放器靠它；马力欧开了读盘变慢（首帧 0.6 → 2.9 s），所以只有需要的游戏才开。`DM_PREEMPT=0` / `1` 可覆盖 |
| `shared_font` | 共享字体地址处放什么：`bfttf`（缺省）= 8 字节 BFTTF 头（「已解」标记）+ TTF，马力欧拿去调 `ScalableFontEngineHelper::Decode`；`ttf` = TTF 本身，超级马力欧 3D 世界 + 狂怒世界（SDK 10）直接交给 `ScalableFontEngine::LoadFont`（带头时引擎报错 403，游戏查字形时崩溃） |
| `device_api` | `nvnDeviceGetInteger` 报的 NVN API 版本 `主.次`（缺省 `55.15`，马力欧要求的）。游戏会核对，不符就不建设备：超级马力欧 3D 世界 + 狂怒世界（SDK 10）要求主版本 53、次版本 ≥ 313。`DM_NVN_API` 可改。不进着色器翻译链版本（只有 `nvn_*` / `shader_*` 键进） |
| `nvn_tex_base`、`nvn_tex_slots`、`nvn_samp_base`、`nvn_img_base`、`nvn_img_slots`、`nvn_ssbo_bases` | NVN 驱动常量段，按全库着色器实测 |
| `shader_groups`、`shader_note` | `shader_split.py` 的来源组与映射表表头说明 |
| `shader_main_glslc` | `on` / `off`（缺省）：`shader_split.py` 另读 `main` 里的 GLSLC 编译输出（魔数 `0x19866891`）。十三机兵防卫圈的着色器全在那里；Engage 的 `main` 里有 11 份用不到的，所以别的游戏不开 |
| `shader_separable` | `on` / `off`（缺省）：游戏建单阶段程序、绘制时按阶段拼，真正的顶点 / 像素配对只在运行时才有。`shader_split.py` 于是给每个顶点着色器配一组「覆盖」——读到的输入合起来等于全部像素着色器读的输入的像素着色器，`dmsh_build.py` 的 `vs_pad` 让每个顶点着色器能接任何像素着色器（十三机兵防卫圈） |

翻译器（`dmsh_xlate/Program.cs`）里仍内置贴图 / 采样器句柄表基址与深度口径。档案的 `nvn_tex_base` / `nvn_samp_base` 与之不同时 `dmsh_build.py` 会停下，所以需要别的值的游戏要先让翻译器可配。

## 加一个游戏

1. **粗查。** 对这个游戏的 NSP 文件夹跑 `scripts/build_ipa_from_nsp.py`。没有档案时它只解出代码模块和 RomFS 清单，写出 `<NSP 文件夹>/<标题号>_survey/survey.md`。报告包括模块与构建号、SDK、引擎与图形接口标记、RomFS 容器、改写点统计、导入与宿主钩子的对比。
2. **档案。** 把 `010015100B514000/game.tsv` 拷成 `dm/games/<标题号>/game.tsv`（目录名用基础版的标题号），填身份、`build` 行和 `modules`。内存与 NVN 的值先沿用马力欧的，等起机时实测再改（[docs/agents](../../docs/agents/README.md)）。
3. **着色器表。** 跑 `DM_GAME=<名> shader_split.py … dm/games/<名>/shader_map.tsv dm/games/<名>/shader_facts.tsv dm/games/<名>/shader_pairs.tsv`。再补空的 `shader_map.tsv.conflicts.tsv`、`bf_want.tsv` 和 `pso_manifest.tsv`（表头照马力欧的抄）。
4. **翻译链。**
   1. `DM_GAME=<名> dmsh_chain.py --header`
   2. `DM_GAME=<名> [IOSOUT=…] dmsh_build.py <包目录> <缓存目录>`
   3. `c1_guard.py <缓存目录>/b-<V> dm/games/<名>/c1_full.tsv`
5. **宿主起机。** 跑 `make -C dm/host GAME=<名>`，然后按 docs/agents 的起机循环走。只适合某一个游戏的代码按它的构建号限定（`dm_main_build()`），不留给所有游戏跑。
6. **IPA。** 用 `GAME=<名> dm/ios/tools/build_ipa.sh`，或者用 NSP 脚本（它现在找得到这个档案了）。
