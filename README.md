# GCLOUD - Dólar

Tema para o [xfce4-screensaver](https://docs.xfce.org/apps/xfce4-screensaver/start): um painel com a
cotação do dólar, o Ibovespa e a previsão do tempo da sua cidade, sobre uma rede de partículas animada.

![Painel com dólar, Ibovespa e previsão do tempo](docs/screenshot.png)

- Dólar comercial e Ibovespa, com a variação do dia e o gráfico dos últimos 30 pregões.
- Gráfico verde em alta e vermelho em queda, com brilho e o ponto de agora em destaque.
- Previsão do tempo da cidade detectada pela conexão (ou de uma cidade fixa).
- Sem chave de API, sem conta e sem dependências além de GTK 3 e Python 3.
- O screensaver não acessa a rede: um coletor separado atualiza os dados a cada 30 minutos.

## Requisitos

- XFCE com o `xfce4-screensaver`, em sessão X11 (veja [Limitações](#limitações)).
- systemd de usuário, para o timer do coletor.
- Testado no Ubuntu 26.04 com o xfce4-screensaver 4.20.1.

## Como funciona

- `gcloud-dolar-fetch` grava `~/.cache/gcloud-dolar/dolar.ini`, com três seções independentes:
  - dólar: AwesomeAPI (comercial); se falhar, Banco Central (PTAX de venda);
  - Ibovespa: Yahoo Finance (`^BVSP`); se falhar, HG Brasil (sem histórico). As duas fontes têm atraso de alguns minutos;
  - tempo: Open-Meteo, na cidade detectada pelo IP da conexão (ipapi.co; se falhar, ipwho.is).

  Se uma seção falhar, a versão antiga dela fica como está.
- Um timer do systemd (de usuário) roda o coletor 30 segundos após o login e depois a cada 30 minutos.
- `gcloud-dolar` é o screensaver. Não acessa a rede: relê o arquivo a cada 30 segundos.
  Se a última coleta bem-sucedida tiver mais de 3 horas, escreve "desatualizado" abaixo do valor
  (e a previsão do tempo some).

## Dependências

Debian, Ubuntu, Mint:

    sudo apt install build-essential pkg-config libgtk-3-dev python3

Fedora:

    sudo dnf install gcc make pkgconf-pkg-config gtk3-devel python3

## Instalação

    ./install.sh

O script pede `sudo` uma vez (ou abre a janela de senha do `pkexec`, se não houver terminal),
para copiar o tema no diretório de temas do sistema, por exemplo `/usr/libexec/xfce4-screensaver`.
O xfce4-screensaver só executa temas que estejam nos diretórios de sistema dele;
se o `Exec=` do `.desktop` apontar para outro lugar (por exemplo `~/.local/bin`),
o tema aparece na lista, mas a tela fica preta. O resto vai para o seu usuário.

Depois abra `xfce4-screensaver-preferences` e escolha "GCLOUD - Dólar".

## Testes

    ~/.local/bin/gcloud-dolar                      # abre numa janela
    ~/.local/bin/gcloud-dolar --snapshot t.png     # grava um quadro em PNG
    ~/.local/bin/gcloud-dolar-fetch                # coleta agora
    systemctl --user list-timers gcloud-dolar.timer
    journalctl --user -u gcloud-dolar.service      # histórico das coletas

Para ver o aviso "desatualizado", edite `atualizado_em` no arquivo de cache para um
horário com mais de 3 horas (é um timestamp Unix).

## Fontes de dados e privacidade

Todas as fontes são públicas e sem chave. Nenhuma tem garantia de disponibilidade, e a do
Yahoo Finance não é uma API oficial; por isso cada dado tem uma fonte reserva.

| Dado | Fonte principal | Reserva |
| --- | --- | --- |
| Dólar | [AwesomeAPI](https://docs.awesomeapi.com.br/api-de-moedas) | [Banco Central (PTAX)](https://dadosabertos.bcb.gov.br/dataset/dolar-americano-usd-todos-os-boletins-diarios) |
| Ibovespa | Yahoo Finance (`^BVSP`) | [HG Brasil](https://hgbrasil.com/status/finance) |
| Localização | [ipapi.co](https://ipapi.co) | [ipwho.is](https://ipwho.is) |
| Previsão do tempo | [Open-Meteo](https://open-meteo.com) | |

O coletor não envia nenhum dado seu além do que toda requisição HTTP carrega: o endereço IP.
É por ele que os serviços de localização descobrem a cidade. Se preferir não usar esses
serviços, fixe a cidade (abaixo): aí o coletor não os consulta.

Os valores são informativos e podem ter atraso. Não use para operar.

## Como ler o gráfico

- A linha são os últimos pregões (a legenda diz quantos) e termina no ponto de agora.
- A cor é a do período mostrado: verde se o último ponto está acima do primeiro, vermelha se
  está abaixo. O percentual da legenda é essa mesma conta. A variação "hoje", logo abaixo do
  valor, tem cor própria e pode ter sinal diferente do gráfico.
- A linha pontilhada é o nível em que o período começou.
- A altura do gráfico cobre no mínimo 3% do valor (`CHART_MIN_SPAN`): um mercado parado
  aparece como uma linha quase reta, e não como um sobe-e-desce ampliado.

## Cidade da previsão do tempo

Por padrão a cidade vem do IP da conexão, que pode errar (VPN, provedor de outra cidade).
Para fixar:

    mkdir -p ~/.config/gcloud-dolar
    echo "Joinville, SC" > ~/.config/gcloud-dolar/cidade
    ~/.local/bin/gcloud-dolar-fetch

O texto do arquivo é o que aparece na tela; a busca usa o que vem antes da vírgula, só no Brasil.
Apague o arquivo para voltar à detecção automática.

## Personalização

No topo de `gcloud-dolar.c`: cores do fundo e da rede (`BG_*`, `AC_*`), cores de alta e queda (`UP_*`, `DN_*`),
limite de desatualização (`STALE_SECONDS`). Recompile com `make` e rode `./install.sh`.

O intervalo de coleta fica em `systemd/gcloud-dolar.timer` (`OnUnitActiveSec`).

## Limitações

- Só sessão X11. O xfce4-screensaver atual embute os temas com `GtkPlug` no X11;
  no Wayland ele usa outra biblioteca (libwlembed), ainda não suportada aqui.
- A variação do dólar é a informada pela fonte (AwesomeAPI) ou a diferença
  para a PTAX anterior (Banco Central). A do Ibovespa é sobre o fechamento anterior.
- A previsão é só a do dia (temperatura atual, máxima, mínima e chance de chuva).

## Desinstalar

    ./uninstall.sh

Remove os programas, o tema, o timer e o cache. A cidade fixada, se houver, fica em
`~/.config/gcloud-dolar/cidade`.

## Licença

[GPL-3.0-or-later](LICENSE). Copyright (C) 2026 Juliano Liberato.
