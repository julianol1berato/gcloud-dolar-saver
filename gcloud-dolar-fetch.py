#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Juliano Liberato
"""Coletor de dados para o screensaver GCLOUD.

Grava ~/.cache/gcloud-dolar/dolar.ini com tres secoes independentes:

  [dolar]  AwesomeAPI (dolar comercial); se falhar, Banco Central (PTAX)
  [ibov]   Yahoo Finance (^BVSP); se falhar, HG Brasil
  [tempo]  Open-Meteo, na cidade detectada pelo IP (ipapi.co; se falhar, ipwho.is)

Para fixar a cidade em vez de detectar pelo IP, escreva o nome dela em
~/.config/gcloud-dolar/cidade (ex.: "Joinville, SC").

Se uma secao falhar, a versao antiga dela fica intacta no arquivo e o
screensaver continua mostrando o ultimo valor disponivel.

So usa a biblioteca padrao do Python 3.
"""

import configparser
import datetime
import json
import os
import sys
import tempfile
import time
import urllib.parse
import urllib.request

HOME = os.path.expanduser("~")
CACHE_DIR = os.path.join(HOME, ".cache", "gcloud-dolar")
CACHE_FILE = os.path.join(CACHE_DIR, "dolar.ini")
CIDADE_FILE = os.path.join(HOME, ".config", "gcloud-dolar", "cidade")

AWESOME_LAST = "https://economia.awesomeapi.com.br/json/last/USD-BRL"
AWESOME_DAILY = "https://economia.awesomeapi.com.br/json/daily/USD-BRL/30"
BCB_BASE = "https://olinda.bcb.gov.br/olinda/servico/PTAX/versao/v1/odata/"
YAHOO_IBOV = "https://query1.finance.yahoo.com/v8/finance/chart/%5EBVSP?range=2mo&interval=1d"
HG_FINANCE = "https://api.hgbrasil.com/finance?format=json"
IPAPI = "https://ipapi.co/json/"
IPWHO = "https://ipwho.is/"
GEOCODE = "https://geocoding-api.open-meteo.com/v1/search?"
FORECAST = "https://api.open-meteo.com/v1/forecast?"

TIMEOUT = 15
HIST_MAX = 30

# codigos de tempo da OMM (WMO 4677), como devolvidos pelo Open-Meteo
WMO = {
    0: "Céu limpo", 1: "Poucas nuvens", 2: "Parcialmente nublado", 3: "Nublado",
    45: "Neblina", 48: "Neblina",
    51: "Garoa fraca", 53: "Garoa", 55: "Garoa forte", 56: "Garoa gelada", 57: "Garoa gelada",
    61: "Chuva fraca", 63: "Chuva", 65: "Chuva forte", 66: "Chuva gelada", 67: "Chuva gelada",
    71: "Neve fraca", 73: "Neve", 75: "Neve forte", 77: "Neve",
    80: "Pancadas de chuva", 81: "Pancadas de chuva", 82: "Pancadas fortes",
    85: "Pancadas de neve", 86: "Pancadas de neve",
    95: "Trovoadas", 96: "Trovoadas com granizo", 99: "Trovoadas com granizo",
}


def http_get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "gcloud-dolar-saver/1.0"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fmt_hist(hist):
    return ";".join("%.4f" % h for h in hist)


# ---------------------------------------------------------------- dolar

def dolar_awesome(_old):
    last = http_get_json(AWESOME_LAST)["USDBRL"]
    valor = float(last["bid"])
    pct = float(last["pctChange"])
    if not 0.5 < valor < 100:
        raise ValueError("valor fora da faixa esperada: %r" % valor)

    hist = []
    try:
        daily = http_get_json(AWESOME_DAILY)
        # a API devolve do mais recente para o mais antigo
        hist = [float(d["bid"]) for d in reversed(daily)][-HIST_MAX:]
    except Exception as exc:  # historico e opcional
        print("aviso: historico da AwesomeAPI falhou: %s" % exc, file=sys.stderr)
    return {"valor": "%.4f" % valor, "pct": "%.4f" % pct, "hist": fmt_hist(hist)}


def dolar_bcb(_old):
    hoje = datetime.date.today()
    inicio = hoje - datetime.timedelta(days=45)
    params = {
        "@dataInicial": "'%s'" % inicio.strftime("%m-%d-%Y"),
        "@dataFinalCotacao": "'%s'" % hoje.strftime("%m-%d-%Y"),
        "$orderby": "dataHoraCotacao asc",
        "$format": "json",
    }
    url = (
        BCB_BASE
        + "CotacaoDolarPeriodo(dataInicial=@dataInicial,dataFinalCotacao=@dataFinalCotacao)?"
        + urllib.parse.urlencode(params, quote_via=urllib.parse.quote, safe="@$'")
    )
    linhas = http_get_json(url)["value"]
    hist = [float(l["cotacaoVenda"]) for l in linhas][-HIST_MAX:]
    if not hist:
        raise ValueError("Banco Central nao devolveu cotacoes no periodo")
    valor = hist[-1]
    if not 0.5 < valor < 100:
        raise ValueError("valor fora da faixa esperada: %r" % valor)
    pct = (valor / hist[-2] - 1) * 100 if len(hist) > 1 else 0.0
    return {"valor": "%.4f" % valor, "pct": "%.4f" % pct, "hist": fmt_hist(hist)}


# ----------------------------------------------------------------- ibov

def check_ibov(valor):
    if not 10000 < valor < 10000000:
        raise ValueError("valor fora da faixa esperada: %r" % valor)


def ibov_yahoo(_old):
    res = http_get_json(YAHOO_IBOV)["chart"]["result"][0]
    valor = float(res["meta"]["regularMarketPrice"])
    check_ibov(valor)
    # fechamentos diarios; o ultimo e o pregao em andamento (ou o mais recente)
    closes = [float(c) for c in res["indicators"]["quote"][0]["close"] if c is not None]
    if len(closes) < 2:
        raise ValueError("historico insuficiente")
    closes[-1] = valor
    pct = (valor / closes[-2] - 1) * 100
    return {"valor": "%.2f" % valor, "pct": "%.4f" % pct, "hist": fmt_hist(closes[-HIST_MAX:])}


def ibov_hg(_old):
    ibov = http_get_json(HG_FINANCE)["results"]["stocks"]["IBOVESPA"]
    valor = float(ibov["points"])
    check_ibov(valor)
    return {"valor": "%.2f" % valor, "pct": "%.4f" % float(ibov["variation"]), "hist": ""}


# ---------------------------------------------------------------- tempo

def local_arquivo():
    """Cidade fixada pelo usuario. Devolve None se o arquivo nao existir."""
    try:
        with open(CIDADE_FILE, encoding="utf-8") as fh:
            nome = fh.read().strip()
    except OSError:
        return None
    if not nome:
        return None
    busca = nome.split(",")[0].strip()
    url = GEOCODE + urllib.parse.urlencode(
        {"name": busca, "count": 1, "language": "pt", "countryCode": "BR"})
    achados = http_get_json(url).get("results") or []
    if not achados:
        raise ValueError("cidade nao encontrada: %r (veja %s)" % (nome, CIDADE_FILE))
    return nome, float(achados[0]["latitude"]), float(achados[0]["longitude"])


def local_ip(url):
    d = http_get_json(url)
    if d.get("error") or d.get("success") is False or not d.get("city"):
        raise ValueError("resposta sem cidade: %s" % (d.get("reason") or d.get("message") or "?"))
    nome = d["city"]
    if d.get("region_code"):
        nome += ", " + d["region_code"]
    return nome, float(d["latitude"]), float(d["longitude"])


def localizar(old):
    fixa = local_arquivo()
    if fixa is not None:
        return fixa
    for url in (IPAPI, IPWHO):
        try:
            return local_ip(url)
        except Exception as exc:
            print("aviso: localizacao por %s falhou: %s" % (url, exc), file=sys.stderr)
    if old.get("cidade") and old.get("lat") and old.get("lon"):
        return old["cidade"], float(old["lat"]), float(old["lon"])
    raise ValueError("nao foi possivel descobrir a localizacao")


def tempo_openmeteo(old):
    cidade, lat, lon = localizar(old)
    url = FORECAST + urllib.parse.urlencode({
        "latitude": "%.4f" % lat,
        "longitude": "%.4f" % lon,
        "current": "temperature_2m,weather_code",
        "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max",
        "timezone": "auto",
        "forecast_days": 1,
    })
    d = http_get_json(url)
    cur, dia = d["current"], d["daily"]
    temp = float(cur["temperature_2m"])
    if not -60 < temp < 60:
        raise ValueError("temperatura fora da faixa esperada: %r" % temp)
    chuva = dia["precipitation_probability_max"][0]
    return {
        "cidade": cidade,
        "lat": "%.4f" % lat,
        "lon": "%.4f" % lon,
        "temp": "%.1f" % temp,
        "descricao": WMO.get(int(cur["weather_code"]), ""),
        "max": "%.1f" % float(dia["temperature_2m_max"][0]),
        "min": "%.1f" % float(dia["temperature_2m_min"][0]),
        "chuva": "" if chuva is None else "%d" % round(float(chuva)),
    }


# ---------------------------------------------------------------- cache

def read_cache():
    cp = configparser.RawConfigParser()
    cp.optionxform = str
    try:
        cp.read(CACHE_FILE, encoding="utf-8")
    except (configparser.Error, UnicodeDecodeError):
        pass
    return {sec: dict(cp.items(sec)) for sec in cp.sections()}


def write_cache(dados):
    os.makedirs(CACHE_DIR, exist_ok=True)
    linhas = []
    for sec, campos in dados.items():
        linhas.append("[%s]" % sec)
        linhas.extend("%s=%s" % (k, str(v).replace("\n", " ")) for k, v in campos.items())
        linhas.append("")
    # grava em arquivo temporario e renomeia: o screensaver nunca le pela metade
    fd, tmp = tempfile.mkstemp(dir=CACHE_DIR, prefix=".dolar-")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write("\n".join(linhas))
        os.replace(tmp, CACHE_FILE)
    except BaseException:
        # disco cheio, sinal no meio da escrita...: nao deixa o temporario para tras
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


SECOES = (
    ("dolar", (("AwesomeAPI", dolar_awesome), ("BCB", dolar_bcb))),
    ("ibov", (("Yahoo", ibov_yahoo), ("HGBrasil", ibov_hg))),
    ("tempo", (("Open-Meteo", tempo_openmeteo),)),
)


def resumo(sec, c):
    if sec == "tempo":
        return "%s, %s°C, %s" % (c["cidade"], c["temp"], c["descricao"])
    return "%s (%+.2f%%)" % (c["valor"], float(c["pct"]))


def main():
    dados = read_cache()
    falhas = 0
    for sec, fontes in SECOES:
        old = dados.get(sec, {})
        for nome, func in fontes:
            try:
                novo = func(old)
            except Exception as exc:
                print("falha em %s/%s: %s" % (sec, nome, exc), file=sys.stderr)
                continue
            if "hist" in novo and not novo["hist"]:
                novo["hist"] = old.get("hist", "")  # fonte sem historico: mantem o antigo
            novo["fonte"] = nome
            novo["atualizado_em"] = "%d" % time.time()
            dados[sec] = novo
            print("ok %s: %s via %s" % (sec, resumo(sec, novo), nome))
            break
        else:
            falhas += 1
            print("%s: todas as fontes falharam; mantendo o ultimo valor salvo" % sec,
                  file=sys.stderr)
    write_cache(dados)
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
