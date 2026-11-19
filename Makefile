CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wshadow -Wformat=2 -Wno-unused-parameter
PKGS     = gtk+-3.0 x11 gl
SAN      = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

gcloud-dolar: gcloud-dolar.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

# binario de teste com AddressSanitizer e UBSan: acusa acesso invalido a
# memoria, estouro e vazamento. Nao e para instalar.
gcloud-dolar-asan: gcloud-dolar.c
	$(CC) $(SAN) $(ASAN_DEFS) -Wall -Wextra -Wno-unused-parameter $$(pkg-config --cflags $(PKGS)) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

asan: gcloud-dolar-asan

# dados malformados x tamanhos de tela, sob os sanitizers
check: gcloud-dolar-asan
	./tests/check.sh

clean:
	rm -f gcloud-dolar gcloud-dolar-asan

.PHONY: asan check clean
