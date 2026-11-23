#!/bin/bash
set -e
cd ~/mu_s23/Mu-Silicium
for p in Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl; do
  echo "=== Submodule: $p ==="
  git submodule update --init --depth 1 -- "$p"
done
echo "=== Creating Python venv ==="
python3 -m venv ~/mu_s23/venv
source ~/mu_s23/venv/bin/activate
pip install -q --upgrade pip
pip install -q -r pip-requirements.txt
echo "MU_ENV_READY!"
