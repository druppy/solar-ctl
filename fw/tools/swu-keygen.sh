#!/bin/sh
# Generate the RSA-4096 keypair used to sign .swu files - and later, with
# the SAME key, to sign U-Boot FIT images for verified boot (that is why
# RSA and not ed25519: neither SWUpdate 2026.05.1 nor U-Boot FIT speaks
# ed25519). Never cat/print the private key here; never commit the result.
set -eu

OUT="${1:-swu-keys}"
if [ -e "$OUT" ]; then
    echo "refusing to overwrite existing $OUT" >&2
    exit 1
fi

mkdir -p "$OUT"
chmod 700 "$OUT"
openssl genrsa -out "$OUT/private.pem" 4096 2>/dev/null
chmod 600 "$OUT/private.pem"
openssl rsa -in "$OUT/private.pem" -pubout -out "$OUT/public.pem" 2>/dev/null
chmod 644 "$OUT/public.pem"

echo "wrote $OUT/private.pem and $OUT/public.pem"
echo
echo "Wire them in (values are accepted as PEM content OR absolute paths):"
echo "  local: .zed/tasks.json 'Build FW' env -> SOLAR_SWU_PRIVATE_KEY (public one optional)"
echo "  CI:    gh secret set SOLAR_SWU_PRIVATE_KEY < $OUT/private.pem"
echo
echo "Without SOLAR_SWU_PRIVATE_KEY the build FAILS on purpose: there is no"
echo "committed fallback keypair - a build can never silently sign with, or"
echo "bake, a key someone left in the repo."
echo "SOLAR_SWU_PUBLIC_KEY may stay unset (derived via 'openssl rsa -pubout');"
echo "if set, it is cross-checked against the private key (mismatch = error)."
