#!/bin/sh
# Instala o screensaver GCLOUD - Dolar para o usuario atual.
# Pede sudo em um unico passo: o xfce4-screensaver so executa temas que estejam
# no diretorio de temas do sistema (ex.: /usr/libexec/xfce4-screensaver). Um Exec= fora
# deles e descartado em silencio e a tela fica preta.
set -eu

cd "$(dirname "$0")"

BINDIR="$HOME/.local/bin"
# o diretorio de temas muda conforme a distribuicao; vale o que tem os temas padrao
SAVERDIR=""
for dir in /usr/libexec/xfce4-screensaver /usr/lib/xfce4-screensaver \
           /usr/lib/x86_64-linux-gnu/xfce4-screensaver /usr/lib64/xfce4-screensaver; do
    if [ -x "$dir/floaters" ]; then
        SAVERDIR="$dir"
        break
    fi
done
if [ -z "$SAVERDIR" ]; then
    echo "erro: nao achei o diretorio de temas do xfce4-screensaver; ele esta instalado?" >&2
    exit 1
fi
DESKTOPDIR="$HOME/.local/share/applications/screensavers"
UNITDIR="$HOME/.config/systemd/user"

echo "1/4 Compilando"
make

echo "2/4 Instalando programas em $BINDIR"
mkdir -p "$BINDIR" "$DESKTOPDIR" "$UNITDIR"
install -m 755 gcloud-dolar "$BINDIR/gcloud-dolar"
install -m 755 gcloud-dolar-fetch.py "$BINDIR/gcloud-dolar-fetch"

echo "3/4 Registrando o tema no xfce4-screensaver (pede sudo para copiar em $SAVERDIR)"
# sem terminal (ex.: rodando de dentro de outro programa) o sudo nao consegue
# pedir a senha; nesse caso usa a janela grafica do pkexec
if [ -t 0 ] || ! command -v pkexec >/dev/null 2>&1; then
    sudo install -m 755 gcloud-dolar "$SAVERDIR/gcloud-dolar"
else
    pkexec install -m 755 "$PWD/gcloud-dolar" "$SAVERDIR/gcloud-dolar"
fi
sed "s|@SAVERDIR@|$SAVERDIR|g" gcloud-dolar.desktop.in > "$DESKTOPDIR/gcloud-dolar.desktop"

echo "4/4 Ativando a atualizacao a cada 30 minutos"
install -m 644 systemd/gcloud-dolar.service "$UNITDIR/gcloud-dolar.service"
install -m 644 systemd/gcloud-dolar.timer "$UNITDIR/gcloud-dolar.timer"
systemctl --user daemon-reload
systemctl --user enable --now gcloud-dolar.timer
systemctl --user start gcloud-dolar.service || echo "aviso: a primeira coleta falhou; veja: journalctl --user -u gcloud-dolar.service"

echo
echo "Pronto. Teste em janela:  $BINDIR/gcloud-dolar"
echo "Depois escolha 'GCLOUD - Dolar' em: xfce4-screensaver-preferences"
