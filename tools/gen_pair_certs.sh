#!/usr/bin/env bash
# gen_pair_certs.sh —— M1 预置证书生成(T15 续; docs/20 §4)
#
# 产出 tools/certs/: 设备 CA(P-256 自签) + 设备服务器证书(SAN 含测试台
# IP/espcube.local) + 测试客户端证书。M1 口径(批次 19 记录): 证书经串口
# CLI 灌入设备 NVS "pairc" 区; **设备端签发**(PSA+自建 DER)归 M2/批次 20
# 与 T16 App 一起定—— mbedtls 4 pk API 已私有化, x509write 不可用。
# 测试 IP 变更时须重生成 server.crt(SAN)。
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p certs && cd certs

DAYS=3650
SUBJ_CA="/CN=EspCube Pair CA O=M1Test"
SUBJ_SRV="/CN=espcube"
SUBJ_CLI="/CN=espcube-test-client"

echo "== CA =="
openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days "$DAYS" \
    -subj "$SUBJ_CA" -out ca.crt \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash"

echo "== server =="
openssl ecparam -name prime256v1 -genkey -noout -out server.key
openssl req -new -key server.key -subj "$SUBJ_SRV" -out server.csr
printf "subjectAltName=IP:192.168.2.120,DNS:espcube.local\n" > srv.ext
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -days "$DAYS" -sha256 -extfile srv.ext -out server.crt
rm -f server.csr srv.ext

echo "== client (测试客户端; App 侧 T16 另行) =="
openssl ecparam -name prime256v1 -genkey -noout -out client.key
openssl req -new -key client.key -subj "$SUBJ_CLI" -out client.csr
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -days "$DAYS" -sha256 -out client.crt
rm -f client.csr

openssl verify -CAfile ca.crt server.crt client.crt
echo "== 产出 =="
ls -la ca.crt server.crt server.key client.crt client.key
