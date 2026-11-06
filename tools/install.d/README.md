# install.d — build steps run by tools/install_app.sh, in name order

Each `NN-name.sh` gets `$ROOT $BUILD $RES` (`$RES` = `.app/Contents/Resources`) and **builds**
what it installs (RULES R-I3: an overlay is built at install time from its sources, never
copied as sources — HP2's wrong-flag bug). Game-derived output (packs, voice) never goes to git.
Example: `20-loc.sh` → `python3 tools/loc/build_mod.py uk --data "$ROOT" --out "$RES/mods"`.
