# Makefile - Práctica domiciliaria Semana 7: servicio concurrente de reservas (C/POSIX)
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -pthread
COMUN    = src/comun.c src/servicio.c src/servicio_defectuoso.c src/carga.c
HEADERS  = src/comun.h src/servicio.h src/carga.h
PROGRAMAS = bin/demo bin/carrera bin/metricas bin/test_reservas

all: $(PROGRAMAS)

bin:
	mkdir -p bin

bin/demo: src/demo.c $(COMUN) $(HEADERS) | bin
	$(CC) $(CFLAGS) -o $@ src/demo.c $(COMUN)

bin/carrera: src/carrera.c $(COMUN) $(HEADERS) | bin
	$(CC) $(CFLAGS) -o $@ src/carrera.c $(COMUN)

bin/metricas: src/metricas.c $(COMUN) $(HEADERS) | bin
	$(CC) $(CFLAGS) -o $@ src/metricas.c $(COMUN)

bin/test_reservas: tests/test_reservas.c $(COMUN) $(HEADERS) | bin
	$(CC) $(CFLAGS) -o $@ tests/test_reservas.c $(COMUN)

# Ejecuta todo en orden
run: all
	./bin/demo
	./bin/carrera
	./bin/metricas
	./bin/test_reservas

test: bin/test_reservas
	./bin/test_reservas

# Las mismas pruebas de carga contra la versión sin sincronización: deben FALLAR
test-defectuoso: bin/test_reservas
	-MODO=defectuoso ./bin/test_reservas

# Detector de carreras de datos (ThreadSanitizer): una corrida de cada versión.
# 'setarch -R' desactiva la aleatorización de memoria, porque en kernels nuevos
# ThreadSanitizer puede fallar al iniciar ("unexpected memory mapping").
TSANFLAGS = -std=c11 -O1 -g -fsanitize=thread -D_POSIX_C_SOURCE=200809L -pthread
tsan: | bin
	$(CC) $(TSANFLAGS) -o bin/carrera_tsan src/carrera.c $(COMUN)
	@cd bin && setarch $$(uname -m) -R ./carrera_tsan --modo defectuoso --reps 1 > /dev/null 2> tsan_defectuoso.txt; true
	@cd bin && setarch $$(uname -m) -R ./carrera_tsan --modo seguro --reps 1 > /dev/null 2> tsan_seguro.txt; true
	@echo "=== Version defectuosa con ThreadSanitizer ==="
	@grep -m1 "FATAL" bin/tsan_defectuoso.txt; true
	@grep SUMMARY bin/tsan_defectuoso.txt | sed 's/(.*//' | sort | uniq -c; true
	@echo "Avisos de data race en la version defectuosa: $$(grep -c 'WARNING: ThreadSanitizer' bin/tsan_defectuoso.txt)"
	@echo "=== Version correcta (mutex + condicion) con ThreadSanitizer ==="
	@grep -m1 "FATAL" bin/tsan_seguro.txt; true
	@echo "Avisos de data race en la version correcta: $$(grep -c 'WARNING: ThreadSanitizer' bin/tsan_seguro.txt)"

# Terminación limpia: sin fugas de memoria ni threads colgados (requiere valgrind)
memcheck: bin/demo
	valgrind --leak-check=full --error-exitcode=1 ./bin/demo > /dev/null

clean:
	rm -rf bin resultados

.PHONY: all run test test-defectuoso tsan memcheck clean
