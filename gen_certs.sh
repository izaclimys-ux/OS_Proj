#!/usr/bin/env bash
# gen_certs.sh — Generate a self-signed TLS certificate for the dashboard
#
# Creates:
#   certs/server.key  — private key  (never leaves the Pi)
#   certs/server.crt  — certificate  (presented to the browser)
#
# The cert is valid for 825 days and includes the Pi's hostname and common
# local IP ranges as Subject Alternative Names so modern browsers accept it.

set -euo pipefail

CERT_DIR="$(dirname "$0")/certs"
KEY_FILE="${CERT_DIR}/server.key"
CRT_FILE="${CERT_DIR}/server.crt"
DAYS=825

mkdir -p "${CERT_DIR}"

# Detect Pi's primary IP (best-effort)
PI_IP=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "127.0.0.1")
PI_HOST=$(hostname 2>/dev/null || echo "raspberrypi")

echo "=== Generating self-signed TLS certificate ==="
echo "    Hostname : ${PI_HOST}"
echo "    IP       : ${PI_IP}"
echo "    Valid    : ${DAYS} days"
echo "    Output   : ${CERT_DIR}/"
echo ""

# Build a temporary OpenSSL config with SANs
TMP_CONF=$(mktemp /tmp/openssl_kb_XXXXXX.cnf)
cat > "${TMP_CONF}" <<OSSL
[req]
default_bits       = 2048
prompt             = no
default_md         = sha256
distinguished_name = dn
x509_extensions    = v3_req

[dn]
C  = SG
ST = Singapore
L  = Singapore
O  = KB Analytics
CN = ${PI_HOST}

[v3_req]
subjectAltName = @alt_names
keyUsage       = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth

[alt_names]
DNS.1 = ${PI_HOST}
DNS.2 = localhost
IP.1  = ${PI_IP}
IP.2  = 127.0.0.1
OSSL

openssl req -x509 -nodes \
    -newkey rsa:2048 \
    -keyout "${KEY_FILE}" \
    -out    "${CRT_FILE}" \
    -days   "${DAYS}" \
    -config "${TMP_CONF}"

rm -f "${TMP_CONF}"

# Lock down the private key
chmod 600 "${KEY_FILE}"
chmod 644 "${CRT_FILE}"

echo ""
echo "=== Certificate generated ==="
echo "    Key  : ${KEY_FILE}"
echo "    Cert : ${CRT_FILE}"
echo ""
echo "Certificate details:"
openssl x509 -in "${CRT_FILE}" -noout -subject -issuer -dates -ext subjectAltName
echo ""
echo "Next step:  python3 dashboard_server.py"
echo "Or just:    sudo ./run.sh  (it calls this automatically)"
