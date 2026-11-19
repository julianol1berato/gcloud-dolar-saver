CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
PKGS     = gtk+-3.0

gcloud-dolar: gcloud-dolar.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

clean:
	rm -f gcloud-dolar

.PHONY: clean
