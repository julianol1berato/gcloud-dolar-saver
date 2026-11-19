#!/bin/sh
# Roda o tema com AddressSanitizer/UBSan contra arquivos de dados malformados,
# em varios tamanhos de tela. Falha se houver acesso invalido a memoria,
# comportamento indefinido ou vazamento.
# Uso: make check
set -eu

cd "$(dirname "$0")/.."
BIN="$PWD/gcloud-dolar-asan"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import sys, time
d, T = sys.argv[1], int(time.time())
big = ";".join("%.4f" % (5 + i * 0.001) for i in range(500))
cases = {
    "vazio": "",
    "lixo": "\x00\xff\xfe isto nao e um ini [[[ ===\n",
    "sem-secao": "valor=5.1\n",
    "um-ponto": "[dolar]\nvalor=5.1\npct=0\nhist=5.1\natualizado_em=%d\n" % T,
    "hist-enorme": "[dolar]\nvalor=5.1\npct=1\nhist=%s\natualizado_em=%d\n"
                   "[ibov]\nvalor=99999999\npct=-99.99\nhist=%s\natualizado_em=%d\n" % (big, T, big, T),
    "hist-igual": "[dolar]\nvalor=5\npct=0\nhist=5;5;5;5\natualizado_em=%d\n" % T,
    "hist-zero": "[dolar]\nvalor=5\npct=0\nhist=0;0;5\natualizado_em=%d\n"
                 "[ibov]\nvalor=1\npct=nan\nhist=abc;def\natualizado_em=xyz\n" % T,
    "extremos": "[dolar]\nvalor=-3\npct=-1\nhist=-1;-2\n"
                "[ibov]\nvalor=1e300\npct=1e300\nhist=1e300;-1e300;inf;nan\natualizado_em=%d\n" % T,
    "textos-longos": "[dolar]\nvalor=5.2\npct=0.1\nfonte=%s\nhist=5;5.2\natualizado_em=%d\n"
                     "[tempo]\ncidade=%s\ntemp=25\ndescricao=%s\nmax=1e9\nmin=-1e9\n"
                     "chuva=999999999999\natualizado_em=%d\n" % ("F" * 500, T, "Cidade " * 200, "Chuva " * 200, T),
    "tempo-sem-cidade": "[tempo]\ntemp=20\natualizado_em=%d\n" % T,
    "datas-absurdas": "[dolar]\nvalor=5.2\npct=0.1\nhist=5;5.2\natualizado_em=99999999999999\n"
                      "[tempo]\ncidade=X\ntemp=20\natualizado_em=-5\n",
    "normal": "[dolar]\nvalor=5.90\npct=0.31\nfonte=AwesomeAPI\nhist=5.0;5.1;5.4;5.5;5.9\natualizado_em=%d\n"
              "[ibov]\nvalor=166570\npct=-0.58\nfonte=Yahoo\nhist=204302;205083;176608;167538;166570\natualizado_em=%d\n"
              "[tempo]\ncidade=Sao Paulo, SP\ntemp=24\ndescricao=Nublado\nmax=27\nmin=18\nchuva=20\natualizado_em=%d\n" % (T, T, T),
}
for name, body in cases.items():
    with open("%s/%s.ini" % (d, name), "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(body)
PY

export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export LSAN_OPTIONS=suppressions=$PWD/tests/lsan.supp:print_suppressions=0

total=0
falhas=0
for f in "$TMP"/*.ini "$TMP/nao-existe.ini"; do
    for tam in "1920 1080" "1080 1920" "320 180" "5120 1440"; do
        total=$((total + 1))
        # shellcheck disable=SC2086
        if ! saida=$(GCLOUD_DOLAR_CACHE="$f" "$BIN" --snapshot "$TMP/out.png" $tam 2>&1) || [ -n "$saida" ]; then
            falhas=$((falhas + 1))
            echo "FALHA: $(basename "$f") em $tam"
            echo "$saida" | head -20
        fi
    done
done

echo "$total execucoes, $falhas falhas"
[ "$falhas" -eq 0 ]
