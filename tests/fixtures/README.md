# Test credentials

No SSH private key is stored in this repository. `scripts/test-qemu.sh` creates
an ephemeral ECDSA P-256 host key under the ignored `.build/` directory for each
test run, derives its SHA-256 fingerprint, and pins that exact value in the UEFI
client command line.
