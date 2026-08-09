#!/usr/bin/env bash
# Fetch id Software DOOM v1.9 shareware (DOOM.EXE + DOOM1.WAD) into
# dosemu/fixtures/doom/ for seeding onto guest C:\APPS (/Dos/Apps).
#
# Shareware IWAD is freely redistributable (John Carmack / id Software).
# Do not modify the extracted binaries. Do not add registered IWADs here.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${REPO_ROOT}/dosemu/fixtures/doom"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

URLS=(
  "https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/doom/doom19s.zip"
  "https://www.gamers.org/pub/idgames/idstuff/doom/doom19s.zip"
)

mkdir -p "${OUT_DIR}"

if [ -f "${OUT_DIR}/DOOM.EXE" ] && [ -f "${OUT_DIR}/DOOM1.WAD" ]; then
  echo "[+] Already present in ${OUT_DIR}"
  ls -la "${OUT_DIR}/DOOM.EXE" "${OUT_DIR}/DOOM1.WAD"
  exit 0
fi

if ! command -v 7z >/dev/null 2>&1 && ! command -v 7za >/dev/null 2>&1; then
  echo "[!] Need 7z/7za to unpack the DEICE split archive (apt install p7zip-full)"
  exit 1
fi
SEVENZ="$(command -v 7z || command -v 7za)"

ZIP="${TMP_DIR}/doom19s.zip"
ok=0
for url in "${URLS[@]}"; do
  echo "[*] Trying ${url}"
  if curl -fL --connect-timeout 20 --max-time 300 -o "${ZIP}" "${url}"; then
    ok=1
    break
  fi
done
if [ "${ok}" -ne 1 ]; then
  echo "[!] Download failed. Place DOOM.EXE and DOOM1.WAD in ${OUT_DIR}/ manually."
  exit 1
fi

unzip -q -o "${ZIP}" -d "${TMP_DIR}/s" 

# doom19s.zip ships DEICE parts: concatenate then 7z (embedded PKZIP stub).
PART1="$(find "${TMP_DIR}/s" -iname 'DOOMS_19.1' | head -1)"
PART2="$(find "${TMP_DIR}/s" -iname 'DOOMS_19.2' | head -1)"
if [ -z "${PART1}" ] || [ -z "${PART2}" ]; then
  echo "[!] Unexpected archive layout"
  find "${TMP_DIR}/s" -type f
  exit 1
fi
cat "${PART1}" "${PART2}" > "${TMP_DIR}/combined.bin"
mkdir -p "${TMP_DIR}/x"
"${SEVENZ}" x -y "${TMP_DIR}/combined.bin" -o"${TMP_DIR}/x" >/dev/null

EXE="$(find "${TMP_DIR}/x" -iname 'DOOM.EXE' | head -1)"
WAD="$(find "${TMP_DIR}/x" -iname 'DOOM1.WAD' | head -1)"
if [ -z "${EXE}" ] || [ -z "${WAD}" ]; then
  echo "[!] DOOM.EXE / DOOM1.WAD missing after extract"
  find "${TMP_DIR}/x" -type f
  exit 1
fi

cp -f "${EXE}" "${OUT_DIR}/DOOM.EXE"
cp -f "${WAD}" "${OUT_DIR}/DOOM1.WAD"
cp -f "${WAD}" "${OUT_DIR}/DOOM.WAD"

cat > "${OUT_DIR}/README.txt" <<'EOF'
DOOM shareware v1.9 (GooberOS bundle)
=====================================

From id Software's freely redistributable doom19s.zip.
Copyright (C) id Software.

  DOOM.EXE   — shareware executable
  DOOM1.WAD  — shareware IWAD (Knee-Deep in the Dead)
  DOOM.WAD   — copy of DOOM1.WAD (alternate name)

Do not modify these files. Do not redistribute registered/commercial IWADs
with GooberOS.

Fetch:   bash scripts/fetch-doom-shareware.sh
Guest:   C:\APPS\DOOM.EXE   (host /Dos/Apps/)
GooberDOS:  CD APPS
            DOOM
EOF

ls -la "${OUT_DIR}/DOOM.EXE" "${OUT_DIR}/DOOM1.WAD"
echo "[+] Staged shareware Doom in ${OUT_DIR}"
