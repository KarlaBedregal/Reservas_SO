/* comun.h
 * Utilidades comunes de la práctica domiciliaria de la Semana 7
 * (Sistemas Operativos - UNSA): tiempo, pausas aleatorias, contexto
 * propio de cada thread y registro de la traza.
 */
#ifndef COMUN_H
#define COMUN_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Revisa el código de retorno de una función pthread. Si falla, muestra
 * dónde y por qué, y termina el programa (manejo de errores explícito). */
#define CHECK_PT(llamada)                                                     \
    do {                                                                      \
        int rc_ = (llamada);                                                  \
        if (rc_ != 0) {                                                       \
            fprintf(stderr, "Error %d (%s) en %s:%d -> %s\n", rc_,            \
                    strerror(rc_), __FILE__, __LINE__, #llamada);             \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------ tiempo */
int64_t ahora_ns(void);                         /* reloj monotónico */
void dormir_us(long us);                        /* pausa en microsegundos */
void limite_desde_ahora(struct timespec *ts, int ms); /* ahora + ms (monotónico) */

/* ------------------------------------------------------------- traza */
typedef enum {
    EV_SOLICITA,      /* pide un recurso */
    EV_ESPERA,        /* no hay recursos libres: se bloquea */
    EV_VERIFICA,      /* encontró un recurso libre (antes de tomarlo) */
    EV_OBTIENE,       /* reserva concedida */
    EV_TIMEOUT,       /* se venció el tiempo de espera */
    EV_LIBERA,        /* cancela su reserva y libera el recurso */
    EV_CANCELADA,     /* su solicitud en espera fue cancelada */
    EV_CERRADO,       /* el servicio está cerrado: rechazada */
    EV_CONSULTA,      /* consulta cuántos recursos libres hay */
    EV_PIDE_CANCELAR, /* (main) pide cancelar la solicitud de otro */
    EV_CIERRE         /* (main) cierra el servicio */
} tipo_evento_t;

typedef struct {
    int64_t t_ns;        /* instante (reloj monotónico) */
    int hilo;            /* 0 = main, 1..N = workers */
    tipo_evento_t tipo;
    int recurso;         /* -1 si no aplica */
    int libres;          /* valor del contador visto en ese momento */
} evento_t;

/* Datos propios de cada thread: no se comparten con nadie. */
typedef struct {
    int id;              /* 0 = main, 1..N */
    unsigned semilla;    /* generador aleatorio propio (rand_r) */
    int max_pausa_us;    /* pausa aleatoria máxima dentro del servicio */
    bool registrar;      /* guardar la traza o no */
    evento_t *eventos;   /* traza local: no necesita lock */
    int n_eventos;
    int cap_eventos;
} hilo_t;

void hilo_init(hilo_t *h, int id, unsigned semilla, int max_pausa_us, bool registrar);
void hilo_destruir(hilo_t *h);
int  hilo_aleatorio(hilo_t *h, int lo, int hi);   /* entero en [lo, hi] */
void hilo_pausa(hilo_t *h);                       /* solo varía intercalados */
void hilo_log(hilo_t *h, tipo_evento_t tipo, int recurso, int libres);

const char *nombre_evento(tipo_evento_t t);
/* Junta las trazas de varios threads y las ordena por tiempo. */
int  unir_trazas(hilo_t *hilos, int n, evento_t **salida);
void imprimir_evento(FILE *f, const evento_t *e, int64_t t0);

#endif
