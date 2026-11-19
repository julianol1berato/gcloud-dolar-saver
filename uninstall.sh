#!/bin/sh
# Remove o screensaver GCLOUD - Dolar do usuario atual.
# Pede sudo (ou a janela do pkexec) para apagar o tema do diretorio de sistema.
set -u

systemctl --user disable --now gcloud-dolar.timer 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/gcloud-dolar.service" \
      "$HOME/.config/systemd/user/gcloud-dolar.timer"
systemctl --user daemon-reload 2>/dev/null || true

rm -f "$HOME/.local/bin/gcloud-dolar" "$HOME/.local/bin/gcloud-dolar-fetch"
rm -f "$HOME/.local/share/applications/screensavers/gcloud-dolar.desktop"
rm -rf "$HOME/.cache/gcloud-dolar"

for dir in /usr/libexec/xfce4-screensaver /usr/lib/xfce4-screensaver \
           /usr/lib/x86_64-linux-gnu/xfce4-screensaver /usr/lib64/xfce4-screensaver; do
    if [ -e "$dir/gcloud-dolar" ]; then
        if [ -t 0 ] || ! command -v pkexec >/dev/null 2>&1; then
            sudo rm -f "$dir/gcloud-dolar"
        else
            pkexec rm -f "$dir/gcloud-dolar"
        fi
    fi
done

echo "Removido. Se voce fixou uma cidade, ela continua em ~/.config/gcloud-dolar/cidade."
