#!/bin/bash
# generate_developer_token.sh — Generate a MusicKit Developer Token (JWT)
# Output: JWT string to stdout (pipe to file or cmake -D)
#
# Usage:
#   ./scripts/generate_developer_token.sh <p8-file> <key-id> <team-id> [expiry-days]
#
# Example:
#   TOKEN=$(./scripts/generate_developer_token.sh AuthKey_4GW6686CH4.p8 4GW6686CH4 W5JMPJXB5H 180)
#   cmake -B build -DMUSICKIT_DEVELOPER_TOKEN="$TOKEN" ..

set -euo pipefail

P8_FILE="${1:-}"
KEY_ID="${2:-}"
TEAM_ID="${3:-}"
EXPIRY_DAYS="${4:-180}"

if [[ -z "$P8_FILE" || -z "$KEY_ID" || -z "$TEAM_ID" ]]; then
    echo "Usage: $0 <p8-file> <key-id> <team-id> [expiry-days]" >&2
    exit 1
fi

[[ -f "$P8_FILE" ]] || { echo "Error: $P8_FILE not found" >&2; exit 1; }

IAT=$(date +%s)
EXP=$((IAT + EXPIRY_DAYS * 86400))

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }

HEADER=$(printf '{"alg":"ES256","kid":"%s"}' "$KEY_ID" | b64url)
PAYLOAD=$(printf '{"iss":"%s","iat":%d,"exp":%d}' "$TEAM_ID" "$IAT" "$EXP" | b64url)

# Sign with ES256 — openssl outputs DER-encoded signature but JWT needs raw R||S.
# DER structure: 30 <len> 02 <r_len> <r_bytes> 02 <s_len> <s_bytes>
# We must extract R and S as zero-padded 32-byte big-endian integers.
SIG_DER=$(printf '%s.%s' "$HEADER" "$PAYLOAD" | openssl dgst -sha256 -sign "$P8_FILE" -binary | xxd -p | tr -d '\n')

# Parse DER: extract R and S integer hex values using openssl asn1parse
TMPDER=$(mktemp)
printf '%s.%s' "$HEADER" "$PAYLOAD" | openssl dgst -sha256 -sign "$P8_FILE" -binary > "$TMPDER"

# asn1parse outputs lines like:
#   0:d=0  hl=2 l=  68 cons: SEQUENCE
#   2:d=1  hl=2 l=  32 prim: INTEGER           :A1B2C3...
#  36:d=1  hl=2 l=  32 prim: INTEGER           :D4E5F6...
R_HEX=$(openssl asn1parse -inform DER -in "$TMPDER" 2>/dev/null | grep INTEGER | head -1 | sed 's/.*://')
S_HEX=$(openssl asn1parse -inform DER -in "$TMPDER" 2>/dev/null | grep INTEGER | tail -1 | sed 's/.*://')
rm -f "$TMPDER"

# Remove leading 00 padding (ASN.1 adds it for positive integers with high bit set)
R_HEX=$(echo "$R_HEX" | sed 's/^00//')
S_HEX=$(echo "$S_HEX" | sed 's/^00//')

# Zero-pad to exactly 64 hex chars (32 bytes) each
R_PAD=$(printf "%064s" "$R_HEX" | tr ' ' '0')
S_PAD=$(printf "%064s" "$S_HEX" | tr ' ' '0')

# Concatenate R||S, convert to binary, base64url encode
SIGNATURE=$(echo -n "${R_PAD}${S_PAD}" | xxd -r -p | b64url)

printf '%s.%s.%s' "$HEADER" "$PAYLOAD" "$SIGNATURE"
