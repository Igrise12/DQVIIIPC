# tools/

Offline tools that run during setup or development: extract/ (disc and archive extraction), ghidra/ (analysis export scripts), recomp/ (recompiler driver and configuration generation), vu1/ (VU1 microprogram compiler), bringup/ (M3 bring-up helpers: frame SSIM, SIF/RPC trace printer; see bringup/README.md).

## Function maps for the recompiler (ghidra/)

`ghidra/export_maps.sh` writes one function map per code unit (`main` and the six overlays) for `ps2_recomp`
by running `ghidra/ExportFunctionMap.java` read-only on the Ghidra projects of spikes S2 and S3
(`work/ghidra/base`, `work/ghidra/s3-<unit>`); unit ranges come from `dq8ctl info --json`. Per unit it
writes `<unit>.csv` (`Name,Start,End,Size`, functions only, End exclusive), `<unit>.jump_tables.toml`
(`[[jump_tables.table]]` keyed by the table's data address) and `<unit>.stats.toml`
(`link_return_entries`, counts, unresolved sites, holes) to `work/recomp/maps/`; the script's header
lists the options and the exporter's header the rules. The maps are game-derived and stay local (brief
§14, D9). About 30 s for all seven.
