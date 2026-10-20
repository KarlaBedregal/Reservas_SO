/* servicio.h
 * Servicio concurrente de reservas sobre M recursos (Semana 7).
 *
 * Varias solicitudes (threads) consultan, reservan, cancelan y esperan
 * disponibilidad. Hay dos implementaciones con la MISMA interfaz:
 *
 *   - servicio.c            -> versión correcta: pthread_mutex_t + pthread_cond_t
 *   - servicio_defectuoso.c -> versión SIN sincronización (para reproducir la carrera)
 *
 * Modelo de estado (protegido por el mutex m, todo junto):
 *   dueno[i]  = -1 si el recurso i está libre, o el id del cliente que lo tiene
 *   libres    = cantidad de recursos libres
 *   cerrado   = el servicio ya no acepta reservas
 *   esperando[c], cancelada[c] = estado de la solicitud del cliente c
 *
 * Invariantes:
 *   I1  libres == cantidad de i con dueno[i] == -1   (contador consistente)
 *   I2  0 <= libres <= M
 *   I3  cada recurso tiene a lo sumo un dueño válido (no hay doble reserva)
 *   I4  reservas - liberaciones == M - libres         (historial coherente)
 *   I5  si cerrado == true no se concede ninguna reserva nueva
 */
#ifndef SERVICIO_H
#define SERVICIO_H

#include "comun.h"

typedef enum {
    RES_OK = 0,      /* reserva concedida / cancelación hecha */
    RES_TIMEOUT,     /* venció el tiempo de espera sin recurso libre */
    RES_CANCELADA,   /* otro thread canceló la solicitud mientras esperaba */
    RES_CERRADO,     /* el servicio está cerrado */
    RES_INVALIDO,    /* argumentos fuera de rango */
    RES_NO_DUENO     /* se intentó cancelar un recurso que no es suyo */
} resultado_t;

const char *nombre_resultado(resultado_t r);

typedef struct {
    /* ---- estado protegido por m ---- */
    int M;
    int *dueno;
    int libres;
    bool cerrado;
    int max_clientes;
    bool *esperando;
    bool *cancelada;
    /* ---- métricas (también protegidas por m) ---- */
    long reservas, liberaciones, timeouts, canceladas, rechazos_cierre, esperas;
    int ocupacion_max;
    long violaciones;            /* invariantes rotos detectados dentro del lock */
    char ultima_violacion[160];
    /* ---- primitivas ---- */
    pthread_mutex_t m;           /* exclusión mutua sobre todo el estado */
    pthread_cond_t cambio;       /* "el estado cambió: revisa tu predicado" */
} servicio_t;

int  servicio_crear(servicio_t *s, int M, int max_clientes);   /* 0 si todo bien */
void servicio_destruir(servicio_t *s);

/* Versión correcta */
resultado_t servicio_reservar(servicio_t *s, hilo_t *h, int cliente, int timeout_ms, int *recurso);
resultado_t servicio_cancelar(servicio_t *s, hilo_t *h, int cliente, int recurso);
int         servicio_consultar(servicio_t *s, hilo_t *h);
bool        servicio_cancelar_solicitud(servicio_t *s, int cliente);
bool        servicio_esta_esperando(servicio_t *s, int cliente);
void        servicio_cerrar(servicio_t *s);

/* Versión defectuosa (sin mutex ni condición) */
resultado_t defectuoso_reservar(servicio_t *s, hilo_t *h, int cliente, int timeout_ms, int *recurso);
resultado_t defectuoso_cancelar(servicio_t *s, hilo_t *h, int cliente, int recurso);
int         defectuoso_consultar(servicio_t *s, hilo_t *h);

/* Revisa I1..I4. Se llama con el mutex tomado o después del join.
 * Devuelve la cantidad de invariantes rotos y deja el detalle en msg. */
int servicio_verificar(const servicio_t *s, char *msg, size_t n);

/* Tabla de operaciones: permite correr la misma carga con cualquiera
 * de las dos versiones sin cambiar el código de las pruebas. */
typedef struct {
    const char *nombre;
    resultado_t (*reservar)(servicio_t *, hilo_t *, int, int, int *);
    resultado_t (*cancelar)(servicio_t *, hilo_t *, int, int);
    int (*consultar)(servicio_t *, hilo_t *);
    bool (*cancelar_solicitud)(servicio_t *, int);   /* NULL si no lo soporta */
} ops_t;

extern const ops_t OPS_SEGURO;
extern const ops_t OPS_DEFECTUOSO;

#endif
