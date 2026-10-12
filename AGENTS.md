# Notes for agents

- This repository ports Super Mario Bros. Wonder (Switch, base game v0) to Apple platforms by running the game's own code natively. Start with [README.md](README.md) for status and build steps.
- To analyse or port **another** Switch title, follow [docs/agents](docs/agents/README.md): extraction, binary survey, host bring-up failure catalogue, graphics specifics, debugging recipes.
- Game files, keys and anything extracted from them live in the user's workspace (`~/smbw-work`), never in this repository. Do not print key values.
- Code comments are bilingual (Chinese line, then English line); red log lines start with `【红】`. Keep both conventions.
- After changing anything on the shader translation chain (`dm/gfx/tools/dmsh_*`, `Program.cs`, `shader_map.tsv`, `shader_facts.tsv`), run `dmsh_chain.py --header`, rebuild the shaders, regenerate `c1_full.tsv`, and rebuild the host: the host only loads libraries of its own chain version.
- Commit only when asked, with `git -c user.name=seedds -c user.email=ext6@xcra.ai commit`.
