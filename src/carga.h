/* carga.h
 * Carga de trabajo concurrente: N workers que consultan, reservan, usan
 * el recurso un momento y lo cancelan. Sirve igual para la versión correcta
 * y para la defectuosa, y la usan las pruebas, la demostración de la carrera
 * y las métricas.
 */
#ifndef CARGA_H
#define CARGA_H

#include "servicio.h"

typedef struct {
    int M;             /* recursos */
    int workers;       /* threads trabajadores (al menos 8) */
    int ops;           /* operaciones por worker */
    int timeout_ms;    /* espera máxima de cada reserva */
    int uso_us;        /* tiempo máximo que se usa un recurso (fuera del servicio) */
    int pausa_us;      /* pausa máxima dentro del servicio (solo varía intercalados) */
    bool cancelador;   /* un thread extra cancela solicitudes en espera al azar */
    bool registrar;    /* guardar traza */
    unsigned semilla;
} config_carga_t;

typedef struct {
    /* conteos */
    long operaciones, consultas, reservas_ok, timeouts, canceladas, cerradas;
    long esperas;               /* reservas que tuvieron que bloquearse */
    /* evidencia de errores (observador externo, con atómicos) */
    long dobles_reservas;       /* veces que dos threads tuvieron el mismo recurso */
    long cancelaciones_fallidas;/* creía tener el recurso pero no era el dueño */
    int  libres_final;
    long violaciones_en_lock;   /* invariantes rotos detectados en cada transición */
    int  errores_finales;       /* invariantes rotos al final */
    char detalle[600];
    /* métricas */
    int  ocupacion_max;
    double tiempo_ms, throughput;
    double espera_prom_ms, espera_p95_ms, espera_max_ms;
    /* traza (si registrar) */
    evento_t *traza;
    int n_traza;
    bool ok;
} resultado_carga_t;

config_carga_t config_por_defecto(void);
resultado_carga_t ejecutar_carga(const ops_t *ops, const config_carga_t *cfg);
void liberar_resultado(resultado_carga_t *r);

/* Busca en la traza el primer recurso concedido a dos threads a la vez y
 * copia en salida los eventos de ese recurso que lo muestran. */
int buscar_doble_reserva(const evento_t *traza, int n, evento_t *salida, int max);

#endif
